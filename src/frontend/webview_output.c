#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <mupdf/fitz.h>

#ifdef __APPLE__
#include <sys/syslimits.h>
#else
#include <linux/limits.h>
#endif

#include "driver.h"
#include "renderer.h"
#include "engine.h"

#include "qoi.h"

static char *g_webview_tmpdir = NULL;

// Incremental render state
static unsigned char *prev_rgb = NULL;
static int prev_w = 0, prev_h = 0;
static int prev_page = -1;

void webview_set_tmpdir(const char *dir)
{
  g_webview_tmpdir = strdup(dir);
}

// Write all bytes to fd, handling partial writes and EINTR
static bool write_all(int fd, const void *data, size_t len)
{
  const unsigned char *p = data;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= n;
  }
  return true;
}

static void write_qoi_file(const char *tmpdir, unsigned char *rgb,
                           int w, int h, char *path_out, size_t path_sz)
{
  qoi_desc desc = { .width = w, .height = h, .channels = 3, .colorspace = QOI_SRGB };
  int qoi_len = 0;
  void *qoi_data = qoi_encode(rgb, &desc, &qoi_len);
  if (!qoi_data) {
    fprintf(stderr, "[webview] ERROR: qoi_encode returned NULL\n");
    return;
  }

  snprintf(path_out, path_sz, "%s/texpresso-XXXXXX", tmpdir);
  int fd = mkstemp(path_out);
  if (fd < 0) {
    fprintf(stderr, "[webview] ERROR: mkstemp(%s) failed: %s\n", path_out, strerror(errno));
    free(qoi_data);
    path_out[0] = '\0';
    return;
  }

  if (!write_all(fd, qoi_data, qoi_len)) {
    fprintf(stderr, "[webview] ERROR: write_all failed: %s\n", strerror(errno));
    close(fd);
    unlink(path_out);
    free(qoi_data);
    path_out[0] = '\0';
    return;
  }
  close(fd);
  free(qoi_data);
}

#define MAX_DIRTY_RECTS 16
#define DIRTY_RATIO_THRESHOLD 0.5f

typedef struct {
  int x, y, w, h;
} dirty_rect_t;

static int compute_dirty_rects(unsigned char *old_rgb, unsigned char *new_rgb,
                                int w, int h, dirty_rect_t *rects, int max_rects,
                                float *dirty_ratio)
{
  int total_pixels = w * h;
  int dirty_pixels = 0;
  int rect_count = 0;

  // For tall pages (>4096px), fall back to full-page update rather
  // than missing changes below the fixed array limit.
  if (h > 4096) {
    *dirty_ratio = 1.0f;
    return -1;
  }

  int row_min_x[4096];
  int row_max_x[4096];
  int dirty_start = -1;

  for (int y = 0; y < h; y++) {
    unsigned char *old_row = old_rgb + y * w * 3;
    unsigned char *new_row = new_rgb + y * w * 3;
    int min_x = w, max_x = -1;

    for (int x = 0; x < w; x++) {
      int idx = x * 3;
      if (old_row[idx] != new_row[idx] ||
          old_row[idx+1] != new_row[idx+1] ||
          old_row[idx+2] != new_row[idx+2]) {
        if (x < min_x) min_x = x;
        max_x = x;
        dirty_pixels++;
      }
    }

    row_min_x[y] = min_x;
    row_max_x[y] = max_x;

    if (max_x >= 0 && dirty_start < 0) {
      dirty_start = y;
    }

    if (dirty_start >= 0 && (max_x < 0 || y == h - 1)) {
      int end_y = (max_x >= 0) ? y : y - 1;

      int rx = w, ry = dirty_start, rw = 0, rh = end_y - dirty_start + 1;
      for (int ry2 = dirty_start; ry2 <= end_y; ry2++) {
        if (row_min_x[ry2] < rx) rx = row_min_x[ry2];
        if (row_max_x[ry2] + 1 - rx > rw) rw = row_max_x[ry2] + 1 - rx;
      }

      if (rw > 0 && rh > 0 && rect_count < max_rects) {
        rects[rect_count].x = rx;
        rects[rect_count].y = ry;
        rects[rect_count].w = rw;
        rects[rect_count].h = rh;
        rect_count++;
      } else if (rect_count >= max_rects) {
        *dirty_ratio = 1.0f;
        return -1;
      }

      dirty_start = -1;
    }
  }

  *dirty_ratio = (float)dirty_pixels / (float)total_pixels;
  return rect_count;
}

// Write a JSON string value safely (escapes ", \, and control chars)
static void write_json_string(FILE *f, const char *s)
{
  putc('"', f);
  for (; *s; s++) {
    unsigned char c = *s;
    if (c == '"' || c == '\\') { putc('\\', f); putc(c, f); }
    else if (c == '\n') { fputs("\\n", f); }
    else if (c == '\r') { fputs("\\r", f); }
    else if (c == '\t') { fputs("\\t", f); }
    else if (c < 0x20) { fprintf(f, "\\u%04X", c); }
    else { putc(c, f); }
  }
  putc('"', f);
}

void webview_output_page(fz_context *ctx, txp_engine *eng,
                         int page, int total_pages,
                         int img_width, int img_height,
                         int page_width, int page_height,
                         const char *tmpdir, bool dark_mode)
{
  if (!tmpdir) tmpdir = g_webview_tmpdir ? g_webview_tmpdir : "/tmp";

  fz_display_list *dl = send(render_page, eng, ctx, page);
  if (!dl) {
    fprintf(stderr, "[webview] ERROR: send(render_page) returned NULL for page %d\n", page);
    return;
  }

  uint32_t bg, fg;
  if (dark_mode) {
    bg = 0x00FFFFFF; fg = 0x00000000;
  } else {
    bg = 0x00000000; fg = 0x00FFFFFF;
  }
  fz_pixmap *pix = txp_renderer_render_to_pixmap(ctx, dl, img_width, img_height, bg, fg);
  fz_drop_display_list(ctx, dl);
  if (!pix) {
    fprintf(stderr, "[webview] ERROR: render_to_pixmap returned NULL\n");
    return;
  }

  unsigned char *samples = fz_pixmap_samples(ctx, pix);
  int w = fz_pixmap_width(ctx, pix);
  int h = fz_pixmap_height(ctx, pix);
  int n = fz_pixmap_components(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);

  unsigned char *rgb = malloc(w * h * 3);
  if (!rgb) {
    fprintf(stderr, "[webview] ERROR: malloc(%d) failed\n", w * h * 3);
    fz_drop_pixmap(ctx, pix);
    return;
  }

  if (stride == w * n && n == 3) {
    memcpy(rgb, samples, (size_t)w * h * 3);
  } else {
    for (int y = 0; y < h; y++) {
      unsigned char *src = samples + stride * y;
      unsigned char *dst = rgb + w * 3 * y;
      for (int x = 0; x < w; x++) {
        dst[x * 3 + 0] = src[x * n + 0];
        dst[x * 3 + 1] = src[x * n + 1];
        dst[x * 3 + 2] = src[x * n + 2];
      }
    }
  }

  fz_drop_pixmap(ctx, pix);

  bool send_update = true;
  bool is_diff = false;
  if (prev_rgb && prev_w == w && prev_h == h && prev_page == page) {
    dirty_rect_t rects[MAX_DIRTY_RECTS];
    float dirty_ratio = 0;
    int n_rects = compute_dirty_rects(prev_rgb, rgb, w, h, rects, MAX_DIRTY_RECTS, &dirty_ratio);
    if (n_rects == 0) {
      send_update = false;
    } else if (n_rects > 0 && dirty_ratio < DIRTY_RATIO_THRESHOLD) {
      is_diff = true;

      // Build a list of successfully prepared rects before emitting JSON
      struct { int x, y, w, h; char path[PATH_MAX]; } emitted[MAX_DIRTY_RECTS];
      int emitted_count = 0;
      for (int i = 0; i < n_rects; i++) {
        dirty_rect_t *r = &rects[i];
        int rw = r->w, rh = r->h;
        unsigned char *rect_rgb = malloc(rw * rh * 3);
        if (!rect_rgb) continue;
        for (int ry = 0; ry < rh; ry++) {
          memcpy(rect_rgb + ry * rw * 3,
                 rgb + ((r->y + ry) * w + r->x) * 3,
                 rw * 3);
        }
        qoi_desc rdesc = { .width = rw, .height = rh, .channels = 3, .colorspace = QOI_SRGB };
        int rqoi_len = 0;
        void *rqoi_data = qoi_encode(rect_rgb, &rdesc, &rqoi_len);
        free(rect_rgb);
        if (!rqoi_data) continue;

        char rpath[PATH_MAX];
        snprintf(rpath, sizeof(rpath), "%s/texpresso-XXXXXX", tmpdir);
        int rfd = mkstemp(rpath);
        if (rfd < 0) { free(rqoi_data); continue; }
        if (!write_all(rfd, rqoi_data, rqoi_len)) {
          close(rfd);
          unlink(rpath);
          free(rqoi_data);
          continue;
        }
        close(rfd);
        free(rqoi_data);

        emitted[emitted_count].x = r->x;
        emitted[emitted_count].y = r->y;
        emitted[emitted_count].w = rw;
        emitted[emitted_count].h = rh;
        memcpy(emitted[emitted_count].path, rpath, sizeof(rpath));
        emitted_count++;
      }

      if (emitted_count > 0) {
        fprintf(stdout, "[\"page-diff\",%d,%d,%d,%d,%d,%d,%d,[",
                page, total_pages, w, h, page_width, page_height, emitted_count);
        for (int i = 0; i < emitted_count; i++) {
          if (i > 0) fprintf(stdout, ",");
          fprintf(stdout, "[%d,%d,%d,%d,",
                  emitted[i].x, emitted[i].y, emitted[i].w, emitted[i].h);
          write_json_string(stdout, emitted[i].path);
          fprintf(stdout, "]");
        }
        fprintf(stdout, "]]\n");
        fflush(stdout);
      }
      // If all rects failed, fall through to full page below
    }
  }

  if (send_update && !is_diff) {
    char tmppath[PATH_MAX];
    write_qoi_file(tmpdir, rgb, w, h, tmppath, sizeof(tmppath));
    if (tmppath[0]) {
      fprintf(stdout, "[\"page\",%d,%d,", page, total_pages);
      write_json_string(stdout, tmppath);
      fprintf(stdout, ",%d,%d,%d,%d]\n", w, h, page_width, page_height);
      fflush(stdout);
    }
  }

  if (prev_rgb) free(prev_rgb);
  prev_rgb = rgb;
  prev_w = w;
  prev_h = h;
  prev_page = page;
}
