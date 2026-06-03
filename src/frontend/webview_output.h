#ifndef WEBVIEW_OUTPUT_H_
#define WEBVIEW_OUTPUT_H_

#include "driver.h"

void webview_state_init(struct webview_state *state);
void webview_state_free(struct webview_state *state);
void webview_state_set_tmpdir(struct webview_state *state, const char *dir);

void webview_output_page(fz_context *ctx, txp_engine *eng,
                         struct webview_state *state,
                         int page, int total_pages,
                         int img_width, int img_height,
                         int page_width, int page_height,
                         bool dark_mode, float trim_factor);

#endif
