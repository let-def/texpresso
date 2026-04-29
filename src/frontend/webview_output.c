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

  ssize_t written = write(fd, qoi_data, qoi_len);
  close(fd);
  free(qoi_data);

  if (written != qoi_len) {
    fprintf(stderr, "[webview] ERROR: write returned %zd, expected %d\n", written, qoi_len);
    unlink(path_out);
    path_out[0] = '\0';
    return;
  }
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
  // Simple scanline-based dirty rect detection
  // We track changed rows and merge into rects
  int total_pixels = w * h;
  int dirty_pixels = 0;
  int rect_count = 0;

  // For each row, find min/max changed column
  int row_min_x[4096]; // max page height ~4000
  int row_max_x[4096];
  int dirty_start = -1;

  if (h > 4096) h = 4096; // safety

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

    // Try to close current rect when row is clean or at end
    if (dirty_start >= 0 && (max_x < 0 || y == h - 1)) {
      int end_y = (max_x >= 0) ? y : y - 1;

      // Find bounding box of dirty region from dirty_start to end_y
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
        // Too many rects, fall back to full page
        *dirty_ratio = 1.0f;
        return -1;
      }

      dirty_start = -1;
    }
  }

  *dirty_ratio = (float)dirty_pixels / (float)total_pixels;
  return rect_count;
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

  // Check if we can do incremental update
  bool send_update = true; // false = no changes, skip sending
  bool is_diff = false;
  if (prev_rgb && prev_w == w && prev_h == h && prev_page == page) {
    dirty_rect_t rects[MAX_DIRTY_RECTS];
    float dirty_ratio = 0;
    int n_rects = compute_dirty_rects(prev_rgb, rgb, w, h, rects, MAX_DIRTY_RECTS, &dirty_ratio);
    if (n_rects == 0) {
      // No changes — skip sending entirely
      send_update = false;
    } else if (n_rects > 0 && dirty_ratio < DIRTY_RATIO_THRESHOLD) {
      is_diff = true;

      // Send page-diff message
      fprintf(stdout, "[\"page-diff\",%d,%d,%d,%d,%d,%d,%d,[",
              page, total_pages, w, h, page_width, page_height, n_rects);

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
        if (rfd >= 0) {
          write(rfd, rqoi_data, rqoi_len);
          close(rfd);
          if (i > 0) fprintf(stdout, ",");
          fprintf(stdout, "[%d,%d,%d,%d,\"%s\"]", r->x, r->y, rw, rh, rpath);
        }
        free(rqoi_data);
      }
      fprintf(stdout, "]]\n");
      fflush(stdout);
    }
    // else: dirty_ratio >= threshold or n_rects < 0 — fall through to full page
  }

  if (send_update && !is_diff) {
    // Full page output
    char tmppath[PATH_MAX];
    write_qoi_file(tmpdir, rgb, w, h, tmppath, sizeof(tmppath));
    if (tmppath[0]) {
      fprintf(stdout, "[\"page\",%d,%d,\"%s\",%d,%d,%d,%d]\n",
              page, total_pages, tmppath, w, h, page_width, page_height);
      fflush(stdout);
    }
  }

  // Save current RGB for next incremental comparison
  if (prev_rgb) free(prev_rgb);
  prev_rgb = rgb;
  prev_w = w;
  prev_h = h;
  prev_page = page;
}
