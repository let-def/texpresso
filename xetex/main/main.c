#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include "tectonic_bridge_core.h"
#include "tectonic_bridge_core_generated.h"
#include "utils.h"
#include "formats.h"
#include "tectonic_provider.h"

/**
 * Global definitions
 */

// Name of root document
char *primary_document = NULL;

// Path of the last opened file
char last_open[PATH_MAX+1];

// Buffer for printing format string
#define FORMAT_BUF_SIZE 4096
char format_buf[FORMAT_BUF_SIZE];

NORETURN PRINTF_FUNC(1,2) int _tt_abort(const char *format, ...)
{
    fprintf(stderr, "Fatal error, aborting: ");
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    do_abort();
}

int ttstub_shell_escape(const unsigned short *cmd, size_t len)
{
    //FIXME: input is UTF-16
    log_proc(logging, "%.*s, %d", (int)len, (char*)cmd, (int)len);
    return 0;
}

// Input management
static FILE *input_as_file(ttbc_input_handle_t *h) { return (void*)h; }
static ttbc_input_handle_t *file_as_input(FILE *h) { return (void*)h; }

ttbc_input_handle_t *ttstub_input_open(const char *path, ttbc_file_format format, int is_gz)
{
    log_proc(logging, "path:%s, format:%s, is_gz:%d", path, ttbc_file_format_to_string(format), is_gz);
    if (is_gz != 0)
        do_abortf("GZ compression not supported");

    FILE *f = fopen(path, "rb");
    const void *buffer = NULL;
    size_t len = 0;
    char tmp[PATH_MAX + 1];

    if (f)
        strcpy(last_open, path);

    if (!f)
    {
        for (const char **exts = format_extensions(format); !f && *exts; exts++)
        {
            strcpy(tmp, path);
            strcat(tmp, *exts);
            f = fopen(tmp, "rb");
            if (f)
                strcpy(last_open, tmp);
        }
    }

    if (!f)
    {
        fprintf(stderr, "input_open: failed to open file %s\n", path);
        fprintf(stderr, "Trying tectonic provider.\n");
        strcpy(tmp, path);
        f = tectonic_get_file(tmp);
        for (const char **exts = format_extensions(format); !f && *exts; exts++)
        {
            strcpy(tmp, path);
            strcat(tmp, *exts);
            f = tectonic_get_file(tmp);
        }
        if (!f)
          fprintf(stderr, "Tectonic failed to provide the file.\n");
    }

    return file_as_input(f);
}

ttbc_input_handle_t *ttstub_input_open_primary(void)
{
    log_proc(logging, "");
    if (!primary_document)
    {
        do_abortf("Document name as not been specified");
    }
    return ttstub_input_open(primary_document, TTBC_FILE_FORMAT_TEX, 0);
}

int ttstub_input_close(ttbc_input_handle_t *handle)
{
    return fclose(input_as_file(handle));
}

int ttstub_input_getc(ttbc_input_handle_t *handle)
{
    return getc(input_as_file(handle));
}

time_t ttstub_input_get_mtime(ttbc_input_handle_t *handle)
{
    struct stat file_stat;
    if (fstat(fileno(input_as_file(handle)), &file_stat) == -1) {
        return 0; // Error getting file statistics
    }
    return file_stat.st_mtime;
}

size_t ttstub_input_get_size(ttbc_input_handle_t *handle)
{
    struct stat file_stat;
    if (fstat(fileno(input_as_file(handle)), &file_stat) == -1) {
        return 0; // Error getting file statistics
    }
    return file_stat.st_size;
}

size_t ttstub_input_seek(ttbc_input_handle_t *handle, ssize_t offset, int whence)
{
    return fseek(input_as_file(handle), offset, whence);
}

ssize_t ttstub_input_read(ttbc_input_handle_t *handle, char *data, size_t len)
{
    return fread(data, 1, len, input_as_file(handle));
}

int ttstub_input_ungetc(ttbc_input_handle_t *handle, int ch)
{
    return ungetc(ch, input_as_file(handle));
}

ssize_t ttstub_get_last_input_abspath(char *buffer, size_t len)
{
    size_t llen = strlen(last_open);
    memmove(buffer, last_open, llen > len ? len : llen);
    if (llen < len)
        buffer[llen] = '\0';
    return llen;
}

// Output management
static FILE *output_as_file(ttbc_output_handle_t *h) { return (void*)h; }
static ttbc_output_handle_t *file_as_output(FILE *h) { return (void*)h; }

int ttstub_output_flush(ttbc_output_handle_t *handle)
{
    return fflush(output_as_file(handle));
}

int ttstub_output_close(ttbc_output_handle_t *handle)
{
    return fclose(output_as_file(handle));
}

ttbc_output_handle_t *ttstub_output_open(char const *path, int is_gz)
{
    log_proc(logging, "path:%s, is_gz:%d", path, is_gz);
    if (is_gz)
        do_abortf("is_gz not supported");

    return file_as_output(fopen(path, "wb"));
}

ttbc_output_handle_t *ttstub_output_open_format(char const *path, int is_gz)
{
    log_proc(logging, "path:%s, is_gz:%d", path, is_gz);
    path = tectonic_cache_path("texpresso-xelatex.fmt");
    if (!path)
        return NULL;
    return file_as_output(fopen(path, "wb"));
}

ttbc_output_handle_t *ttstub_output_open_stdout(void)
{
    return file_as_output(stdout);
}

int ttstub_output_putc(ttbc_output_handle_t *handle, int c)
{
    return putc(c, output_as_file(handle));
}

size_t ttstub_output_write(ttbc_output_handle_t *handle, const char *data, size_t len)
{
    return fwrite(data, 1, len, output_as_file(handle));
}

PRINTF_FUNC(2,3) int ttstub_fprintf(ttbc_output_handle_t *handle, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int len = vsnprintf(format_buf, FORMAT_BUF_SIZE, format, ap);
    va_end(ap);
    return ttstub_output_write(handle, format_buf, len);
}

// Error diagnostics

enum
{
  DIAG_NONE,
  DIAG_WARN,
  DIAG_ERROR,
} diag_kind;

char diag_buf[1024];
size_t diag_len = 0;
intptr_t curr_diag = 0;

static void diag_write(ttbc_diagnostic_t *diag, const char *buf, ssize_t len)
{
    if ((intptr_t)diag != curr_diag)
        do_abortf("diagnostic: use after free");
    if (diag_len + len > sizeof(diag_buf))
        len = sizeof(diag_buf) - diag_len;
    memmove(diag_buf + diag_len, buf, len);
    diag_len += len;
}

void ttbc_diag_append(ttbc_diagnostic_t *diag, const char *text)
{
    diag_write(diag, text, strlen(text));
}

ttbc_diagnostic_t *ttbc_diag_begin_error(void)
{
    if (diag_kind != DIAG_NONE)
        do_abortf("diagnostic: unexpected nested diagnostics");
    diag_kind = DIAG_ERROR;
    diag_len = 0;
    return (void*)(++curr_diag);
}

ttbc_diagnostic_t *ttbc_diag_begin_warning(void)
{
    if (diag_kind != DIAG_NONE)
        do_abortf("Unexpected nested diagnostics");
    diag_kind = DIAG_WARN;
    diag_len = 0;
    return (void*)(++curr_diag);
}

void ttstub_diag_finish(ttbc_diagnostic_t *diag)
{
    if ((intptr_t)diag != curr_diag)
        do_abortf("diagnostic: use after free");
    switch (diag_kind)
    {
        case DIAG_WARN:
            fprintf(stderr, "Warning: ");
            break;

        case DIAG_ERROR:
            fprintf(stderr, "Error: ");
            break;

        default:
            do_abortf("diagnostic: double free");
    }
    diag_kind = DIAG_NONE;

    fprintf(stderr, "%.*s\n", (int)diag_len, diag_buf);
}

static void diag_vprintf(ttbc_diagnostic_t *diag, const char *format, va_list ap)
{
    diag_write(diag, format_buf, vsnprintf(format_buf, FORMAT_BUF_SIZE, format, ap));
}

PRINTF_FUNC(2,3) void ttstub_diag_printf(ttbc_diagnostic_t *diag, const char *format, ...)
{
    log_proc(logging, "\"%s\", ...", format);
    va_list ap;
    va_start(ap, format);
    diag_vprintf(diag, format, ap);
    va_end(ap);
}

// Error helpers
PRINTF_FUNC(1,2) void ttstub_issue_warning(const char *format, ...)
{
    ttbc_diagnostic_t *diag = ttbc_diag_begin_warning();
    va_list ap;
    va_start(ap, format);
    diag_vprintf(diag, format, ap);
    va_end(ap);
    ttstub_diag_finish(diag);
}

PRINTF_FUNC(1,2) void ttstub_issue_error(const char *format, ...)
{
    ttbc_diagnostic_t *diag = ttbc_diag_begin_error();
    va_list ap;
    va_start(ap, format);
    diag_vprintf(diag, format, ap);
    va_end(ap);
    ttstub_diag_finish(diag);
}

// Entry point

// Generate everything based on 1738978143
// (February 8, 2025)
#define EXECUTION_DATE 1738978143

int main(int argc, char **argv)
{
    // Generate a format file
    const char *path = tectonic_cache_path("texpresso-xelatex.fmt");
    if (access(path, R_OK) != 0)
    {
      in_initex_mode = true;
      primary_document = "xelatex.ini";
      tt_history_t result =
          tt_run_engine("texpresso-xelatex.fmt", "xelatex.ini", EXECUTION_DATE);

      fprintf(stderr, "Format generation: ");
      switch (result)
      {
        case HISTORY_SPOTLESS:
          fprintf(stderr, "Spotless execution.\n");
          break;
        case HISTORY_WARNING_ISSUED:
          fprintf(stderr, "Warnings issued.\n");
          break;
        case HISTORY_ERROR_ISSUED:
          fprintf(stderr, "Errors issued.\n");
          break;
        case HISTORY_FATAL_ERROR:
          fprintf(stderr, "Aborted with a fatal error.\n");
          break;
      }
      if (result != HISTORY_SPOTLESS)
        return 1;
    }

    if (argc == 1)
        return 0;

    in_initex_mode = false;
    primary_document = argv[1];
    tt_history_t result =
        tt_run_engine("texpresso-xelatex.fmt", primary_document, EXECUTION_DATE);

    fprintf(stderr, "Format generation: ");
    switch (result)
    {
      case HISTORY_SPOTLESS:
        fprintf(stderr, "Spotless execution.\n");
        break;
      case HISTORY_WARNING_ISSUED:
        fprintf(stderr, "Warnings issued.\n");
        break;
      case HISTORY_ERROR_ISSUED:
        fprintf(stderr, "Errors issued.\n");
        break;
      case HISTORY_FATAL_ERROR:
        fprintf(stderr, "Aborted with a fatal error.\n");
        break;
    }
    if (result == HISTORY_SPOTLESS)
      return 0;
    return 1;
}
