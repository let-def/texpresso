#ifndef WEBVIEW_OUTPUT_H_
#define WEBVIEW_OUTPUT_H_

#include "driver.h"

void webview_output_page(fz_context *ctx, txp_engine *eng,
                         int page, int total_pages,
                         int img_width, int img_height,
                         int page_width, int page_height,
                         const char *tmpdir, bool dark_mode);

void webview_set_tmpdir(const char *dir);

#endif
