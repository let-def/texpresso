#ifndef WEBVIEW_OUTPUT_H_
#define WEBVIEW_OUTPUT_H_

#include "driver.h"

void webview_state_init(struct webview_state *state);
void webview_state_free(struct webview_state *state);
bool webview_state_set_tmpdir(struct webview_state *state, const char *dir);

bool webview_output_page(fz_context *ctx, fz_display_list *dl,
                         struct webview_state *state,
                         int page, int total_pages,
                         int img_width, int img_height,
                         int page_width, int page_height,
                         bool dark_mode, float trim_factor);

#endif
