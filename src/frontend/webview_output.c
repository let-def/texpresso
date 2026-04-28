#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

void webview_set_tmpdir(const char *dir)
{
  g_webview_tmpdir = strdup(dir);
}

void webview_output_page(fz_context *ctx, txp_engine *eng,
                         int page, int total_pages,
                         int img_width, int img_height,
                         const char *tmpdir)
{
  if (!tmpdir) tmpdir = g_webview_tmpdir ? g_webview_tmpdir : "/tmp";

  fz_display_list *dl = send(render_page, eng, ctx, page);
  if (!dl) return;

  fz_pixmap *pix = txp_renderer_render_to_pixmap(ctx, dl, img_width, img_height,
                                                   0x00FFFFFF, 0x00000000);
  fz_drop_display_list(ctx, dl);
  if (!pix) return;

  unsigned char *samples = fz_pixmap_samples(ctx, pix);
  int w = fz_pixmap_width(ctx, pix);
  int h = fz_pixmap_height(ctx, pix);
  int n = fz_pixmap_components(ctx, pix);

  // QOI expects RGBA, but pixmap is BGR (3 channels). Convert BGR to RGB.
  int stride = fz_pixmap_stride(ctx, pix);
  unsigned char *rgb = malloc(w * h * 3);
  if (!rgb) { fz_drop_pixmap(ctx, pix); return; }

  for (int y = 0; y < h; y++) {
    unsigned char *src = samples + stride * y;
    unsigned char *dst = rgb + w * 3 * y;
    for (int x = 0; x < w; x++) {
      dst[x * 3 + 0] = src[x * n + 2]; // R
      dst[x * 3 + 1] = src[x * n + 1]; // G
      dst[x * 3 + 2] = src[x * n + 0]; // B
    }
  }

  qoi_desc desc = { .width = w, .height = h, .channels = 3, .colorspace = QOI_SRGB };
  int qoi_len = 0;
  void *qoi_data = qoi_encode(rgb, &desc, &qoi_len);
  free(rgb);
  fz_drop_pixmap(ctx, pix);

  if (!qoi_data) return;

  // Write to temp file
  char tmppath[PATH_MAX];
  snprintf(tmppath, sizeof(tmppath), "%s/texpresso-XXXXXX.qoi", tmpdir);
  int fd = mkstemp(tmppath);
  if (fd < 0) { free(qoi_data); return; }

  ssize_t written = write(fd, qoi_data, qoi_len);
  close(fd);
  free(qoi_data);

  if (written != qoi_len) { unlink(tmppath); return; }

  // Output JSON to stdout
  fprintf(stdout, "[\"page\",%d,%d,\"%s\",%d,%d]\n",
          page, total_pages, tmppath, w, h);
  fflush(stdout);
}
