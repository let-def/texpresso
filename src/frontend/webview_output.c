#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <limits.h>
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

void webview_state_init(struct webview_state *state)
{
  state->prev_rgb = NULL;
  state->prev_w = 0;
  state->prev_h = 0;
  state->prev_page = -1;
  state->tmpdir = NULL;
}

void webview_state_free(struct webview_state *state)
{
  if (state->prev_rgb) free(state->prev_rgb);
  if (state->tmpdir) free(state->tmpdir);
  webview_state_init(state);
}

bool webview_state_set_tmpdir(struct webview_state *state, const char *dir)
{
  char *copy = dir ? strdup(dir) : NULL;
  if (dir && !copy)
    return false;
  if (state->tmpdir) free(state->tmpdir);
  state->tmpdir = copy;
  return true;
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
    if (n == 0)
      return false;
    p += n;
    len -= n;
  }
  return true;
}

static bool make_temp_template(const char *tmpdir, char *path, size_t path_size)
{
  int written = snprintf(path, path_size, "%s/texpresso-XXXXXX", tmpdir);
  if (written < 0 || (size_t)written >= path_size) {
    fprintf(stderr, "[webview] ERROR: temporary output path is too long\n");
    if (path_size > 0)
      path[0] = '\0';
    return false;
  }
  return true;
}

static bool write_qoi_file(const char *tmpdir, unsigned char *rgb,
                           int w, int h, char *path_out, size_t path_sz)
{
  if (path_sz > 0)
    path_out[0] = '\0';

  qoi_desc desc = { .width = w, .height = h, .channels = 3, .colorspace = QOI_SRGB };
  int qoi_len = 0;
  void *qoi_data = qoi_encode(rgb, &desc, &qoi_len);
  if (!qoi_data) {
    fprintf(stderr, "[webview] ERROR: qoi_encode returned NULL\n");
    return false;
  }

  if (!make_temp_template(tmpdir, path_out, path_sz)) {
    free(qoi_data);
    return false;
  }
  int fd = mkstemp(path_out);
  if (fd < 0) {
    fprintf(stderr, "[webview] ERROR: mkstemp(%s) failed: %s\n", path_out, strerror(errno));
    free(qoi_data);
    path_out[0] = '\0';
    return false;
  }

  if (!write_all(fd, qoi_data, qoi_len)) {
    fprintf(stderr, "[webview] ERROR: write_all failed: %s\n", strerror(errno));
    close(fd);
    unlink(path_out);
    free(qoi_data);
    path_out[0] = '\0';
    return false;
  }
  close(fd);
  free(qoi_data);
  return true;
}

/* Bound temporary-file/message fan-out for a single frame update. */
#define MAX_DIRTY_RECTS 16
/* Above half a page, one full QOI is cheaper than many small QOIs. */
#define DIRTY_RATIO_THRESHOLD 0.5f
typedef struct {
  int x, y, w, h;
} dirty_rect_t;

static int compute_dirty_rects(unsigned char *old_rgb, unsigned char *new_rgb,
                                int w, int h, dirty_rect_t *rects, int max_rects,
                                float *dirty_ratio)
{
  size_t total_pixels = (size_t)w * (size_t)h;
  size_t dirty_pixels = 0;
  int rect_count = 0;
  int dirty_start = -1;
  int dirty_min_x = w;
  int dirty_max_x = -1;
  size_t row_bytes = (size_t)w * 3;

  for (int y = 0; y < h; y++) {
    unsigned char *old_row = old_rgb + (size_t)y * row_bytes;
    unsigned char *new_row = new_rgb + (size_t)y * row_bytes;
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

    if (max_x >= 0 && dirty_start < 0) {
      dirty_start = y;
      dirty_min_x = min_x;
      dirty_max_x = max_x;
    } else if (max_x >= 0) {
      if (min_x < dirty_min_x) dirty_min_x = min_x;
      if (max_x > dirty_max_x) dirty_max_x = max_x;
    }

    if (dirty_start >= 0 && (max_x < 0 || y == h - 1)) {
      int end_y = (max_x >= 0) ? y : y - 1;
      int rx = dirty_min_x;
      int ry = dirty_start;
      int rw = dirty_max_x - dirty_min_x + 1;
      int rh = end_y - dirty_start + 1;

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
      dirty_min_x = w;
      dirty_max_x = -1;
    }
  }

  *dirty_ratio = (float)((double)dirty_pixels / (double)total_pixels);
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

// Resolve tmpdir: explicit -> state->tmpdir -> $TMPDIR -> /tmp
static const char *resolve_tmpdir(struct webview_state *state, const char *explicit_dir)
{
  if (explicit_dir && explicit_dir[0])
    return explicit_dir;
  if (state->tmpdir && state->tmpdir[0])
    return state->tmpdir;
  const char *env = getenv("TMPDIR");
  if (env && env[0])
    return env;
  return "/tmp";
}

static bool trimmed_dimension(int base, float trim_factor, int *result)
{
  if (trim_factor <= 0.0f)
  {
    *result = base;
    return true;
  }

  double zoom = 1.0 / (1.0 - 2.0 * (double)trim_factor);
  double scaled = ceil((double)base * zoom);
  if (!isfinite(scaled) || scaled < base || scaled > INT_MAX - 1)
    return false;

  int value = (int)scaled;
  /* Match parity so the center crop has an integral offset on both sides. */
  if (((value - base) & 1) != 0)
    value++;
  *result = value;
  return true;
}

bool webview_output_page(fz_context *ctx, fz_display_list *dl,
                         struct webview_state *state,
                         int page, int total_pages,
                         int img_width, int img_height,
                         int page_width, int page_height,
                         bool dark_mode, float trim_factor)
{
  const char *tmpdir = resolve_tmpdir(state, NULL);

  if (!dl) {
    fprintf(stderr, "[webview] ERROR: display list is NULL for page %d\n", page);
    return false;
  }
  if (img_width <= 0 || img_height <= 0 ||
      page_width <= 0 || page_height <= 0) {
    fprintf(stderr, "[webview] ERROR: invalid dimensions for page %d\n", page);
    return false;
  }

  if (!isfinite(trim_factor) || trim_factor < 0.0f)
  {
    fprintf(stderr, "[webview] ERROR: invalid trim factor\n");
    return false;
  }
  if (trim_factor > TXP_MAX_TRIM_FACTOR)
    trim_factor = TXP_MAX_TRIM_FACTOR;

  /* These colors describe where source black and white are mapped. */
  uint32_t black_color, white_color;
  if (dark_mode) {
    black_color = 0x00FFFFFF;
    white_color = 0x00000000;
  } else {
    black_color = 0x00000000;
    white_color = 0x00FFFFFF;
  }

  // Trim: render at zoomed resolution, then crop center to output size.
  // This clips all four sides equally without touching the renderer.
  fz_pixmap *pix = NULL;
  int trim_w = img_width, trim_h = img_height;
  if (!trimmed_dimension(img_width, trim_factor, &trim_w) ||
      !trimmed_dimension(img_height, trim_factor, &trim_h) ||
      (size_t)trim_w > SIZE_MAX / (size_t)trim_h / 3) {
    fprintf(stderr, "[webview] ERROR: trimmed render dimensions are out of range\n");
    return false;
  }
  pix = txp_renderer_render_to_pixmap(ctx, dl, trim_w, trim_h,
                                      black_color, white_color);
  if (pix && (trim_w != img_width || trim_h != img_height)) {
    // Crop center of zoomed pixmap back to output size
    int crop_x = (trim_w - img_width) / 2;
    int crop_y = (trim_h - img_height) / 2;
    fz_pixmap *cropped = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx),
                                                   fz_make_irect(0, 0, img_width, img_height),
                                                   NULL, 0);
    if (cropped) {
      fz_clear_pixmap_with_value(ctx, cropped, 0xFF);
      // Copy center portion
      unsigned char *src_samples = fz_pixmap_samples(ctx, pix);
      unsigned char *dst_samples = fz_pixmap_samples(ctx, cropped);
      int src_stride = fz_pixmap_stride(ctx, pix);
      int dst_stride = fz_pixmap_stride(ctx, cropped);
      int n = fz_pixmap_components(ctx, pix);
      if (n != 3 || src_stride <= 0 || dst_stride <= 0 ||
          (size_t)src_stride < (size_t)trim_w * (size_t)n ||
          (size_t)dst_stride < (size_t)img_width * (size_t)n)
      {
        fprintf(stderr, "[webview] ERROR: renderer did not return packed RGB\n");
        fz_drop_pixmap(ctx, cropped);
        fz_drop_pixmap(ctx, pix);
        return false;
      }
      for (int y = 0; y < img_height; y++) {
        memcpy(dst_samples + (size_t)y * (size_t)dst_stride,
               src_samples + (size_t)(crop_y + y) * (size_t)src_stride +
                   (size_t)crop_x * (size_t)n,
               (size_t)img_width * (size_t)n);
      }
      fz_drop_pixmap(ctx, pix);
      pix = cropped;
    }
  }
  if (!pix) {
    fprintf(stderr, "[webview] ERROR: render_to_pixmap returned NULL\n");
    return false;
  }

  unsigned char *samples = fz_pixmap_samples(ctx, pix);
  int w = fz_pixmap_width(ctx, pix);
  int h = fz_pixmap_height(ctx, pix);
  int n = fz_pixmap_components(ctx, pix);
  int stride = fz_pixmap_stride(ctx, pix);

  /* txp_renderer_render_to_pixmap creates device-RGB pixmaps without alpha. */
  if (w <= 0 || h <= 0 || n != 3 || stride <= 0 ||
      (size_t)stride < (size_t)w * (size_t)n ||
      (size_t)w > SIZE_MAX / (size_t)h / 3) {
    fprintf(stderr, "[webview] ERROR: invalid rendered pixmap dimensions\n");
    fz_drop_pixmap(ctx, pix);
    return false;
  }
  size_t rgb_size = (size_t)w * (size_t)h * 3;
  unsigned char *rgb = malloc(rgb_size);
  if (!rgb) {
    fprintf(stderr, "[webview] ERROR: malloc(%zu) failed\n", rgb_size);
    fz_drop_pixmap(ctx, pix);
    return false;
  }

  size_t row_bytes = (size_t)w * 3;
  if ((size_t)stride == (size_t)w * (size_t)n) {
    memcpy(rgb, samples, rgb_size);
  } else {
    for (int y = 0; y < h; y++) {
      unsigned char *src = samples + (size_t)stride * (size_t)y;
      unsigned char *dst = rgb + row_bytes * (size_t)y;
      memcpy(dst, src, row_bytes);
    }
  }

  fz_drop_pixmap(ctx, pix);

  int *prev_w = &state->prev_w;
  int *prev_h = &state->prev_h;
  int *prev_page = &state->prev_page;
  unsigned char **prev_rgb = &state->prev_rgb;

  bool send_update = true;
  bool is_diff = false;
  bool page_sent = false;
  /* The first implementation intentionally caches only the latest page. */
  if (*prev_rgb && *prev_w == w && *prev_h == h && *prev_page == page) {
    dirty_rect_t rects[MAX_DIRTY_RECTS];
    float dirty_ratio = 0;
    int n_rects = compute_dirty_rects(*prev_rgb, rgb, w, h, rects, MAX_DIRTY_RECTS, &dirty_ratio);
    if (n_rects == 0) {
      send_update = false;
    } else if (n_rects > 0 && dirty_ratio < DIRTY_RATIO_THRESHOLD) {
      is_diff = true;

      struct { int x, y, w, h; char path[PATH_MAX]; } emitted[MAX_DIRTY_RECTS];
      int emitted_count = 0;
      for (int i = 0; i < n_rects; i++) {
        dirty_rect_t *r = &rects[i];
        int rw = r->w, rh = r->h;
        if (rw <= 0 || rh <= 0 ||
            (size_t)rw > SIZE_MAX / (size_t)rh / 3)
          continue;
        size_t rect_row_bytes = (size_t)rw * 3;
        size_t rect_rgb_size = rect_row_bytes * (size_t)rh;
        unsigned char *rect_rgb = malloc(rect_rgb_size);
        if (!rect_rgb) continue;
        for (int ry = 0; ry < rh; ry++) {
          memcpy(rect_rgb + (size_t)ry * rect_row_bytes,
                 rgb + ((size_t)(r->y + ry) * (size_t)w +
                        (size_t)r->x) * 3,
                 rect_row_bytes);
        }
        qoi_desc rdesc = { .width = rw, .height = rh, .channels = 3, .colorspace = QOI_SRGB };
        int rqoi_len = 0;
        void *rqoi_data = qoi_encode(rect_rgb, &rdesc, &rqoi_len);
        free(rect_rgb);
        if (!rqoi_data) continue;

        char rpath[PATH_MAX];
        if (!make_temp_template(tmpdir, rpath, sizeof(rpath))) {
          free(rqoi_data);
          continue;
        }
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
        memcpy(emitted[emitted_count].path, rpath, strlen(rpath) + 1);
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
        page_sent = true;
      } else {
        // All dirty rect encodes failed (e.g. disk full) —
        // fall back to full page send below.
        is_diff = false;
      }
    }
  }

  if (send_update && !is_diff) {
    char tmppath[PATH_MAX];
    if (write_qoi_file(tmpdir, rgb, w, h, tmppath, sizeof(tmppath))) {
      fprintf(stdout, "[\"page\",%d,%d,", page, total_pages);
      write_json_string(stdout, tmppath);
      fprintf(stdout, ",%d,%d,%d,%d]\n", w, h, page_width, page_height);
      fflush(stdout);
      page_sent = true;
    }
  }

  // Only save state when a page was actually sent, to prevent permanent desync
  if (page_sent || !*prev_rgb) {
    if (*prev_rgb) free(*prev_rgb);
    *prev_rgb = rgb;
    *prev_w = w;
    *prev_h = h;
    *prev_page = page;
  } else {
    free(rgb);
  }
  return page_sent;
}
