#include "tectonic_bridge_core.h"
#include "tectonic_bridge_core_generated.h"

// TODO
int main(int argc, char **argv);
NORETURN PRINTF_FUNC(1,2) int _tt_abort(const char *format, ...);
void ttbc_diag_append(ttbc_diagnostic_t *diag, const char *text);
ttbc_diagnostic_t *ttbc_diag_begin_error(void);
ttbc_diagnostic_t *ttbc_diag_begin_warning(void);
void ttstub_diag_finish(ttbc_diagnostic_t *diag);
PRINTF_FUNC(2,3) void ttstub_diag_printf(ttbc_diagnostic_t *diag, const char *format, ...);
PRINTF_FUNC(2,3) int ttstub_fprintf(ttbc_output_handle_t *handle, const char *format, ...);
ssize_t ttstub_get_last_input_abspath(char *buffer, size_t len);
int ttstub_input_close(ttbc_input_handle_t *handle);
int ttstub_input_getc(ttbc_input_handle_t *handle);
time_t ttstub_input_get_mtime(ttbc_input_handle_t *handle);
size_t ttstub_input_get_size(ttbc_input_handle_t *handle);
ttbc_input_handle_t *ttstub_input_open(char const *path, ttbc_file_format format, int is_gz);
ttbc_input_handle_t *ttstub_input_open_primary(void);
size_t ttstub_input_seek(ttbc_input_handle_t *handle, ssize_t offset, int whence);
ssize_t ttstub_input_read(ttbc_input_handle_t *handle, char *data, size_t len);
int ttstub_input_ungetc(ttbc_input_handle_t *handle, int ch);
PRINTF_FUNC(1,2) void ttstub_issue_warning(const char *format, ...);
PRINTF_FUNC(1,2) void ttstub_issue_error(const char *format, ...);
int ttstub_output_flush(ttbc_output_handle_t *handle);
int ttstub_output_close(ttbc_output_handle_t *handle);
ttbc_output_handle_t *ttstub_output_open(char const *path, int is_gz);
ttbc_output_handle_t *ttstub_output_open_stdout(void);
int ttstub_output_putc(ttbc_output_handle_t *handle, int c);
size_t ttstub_output_write(ttbc_output_handle_t *handle, const char *data, size_t len);
int ttstub_shell_escape(const unsigned short *cmd, size_t len);
