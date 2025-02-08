#include <linux/limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include "tectonic_bridge_core.h"
#include "tectonic_bridge_core_generated.h"

/**
 * Global definitions
 */

// Is logging enabled
bool logging = 1;

// Name of root document
char *primary_document = NULL;

// Path of the last opened file
char last_open[PATH_MAX+1];

// Buffer for printing format string
#define FORMAT_BUF_SIZE 4096
char format_buf[FORMAT_BUF_SIZE];

/**
 * @brief Logging helper functions for debugging and error handling.
 */

#include <execinfo.h>
#define BT_BUF_SIZE 100

/**
 * @brief Prints a backtrace to stderr.
 *
 * This function captures the current call stack and prints it to the standard error output.
 * It uses the `backtrace` and `backtrace_symbols` functions to retrieve and format the stack trace.
 */
static void print_backtrace(void)
{
    int nptrs;
    void *buffer[BT_BUF_SIZE];
    char **strings;

    nptrs = backtrace(buffer, BT_BUF_SIZE);
    fprintf(stderr, "backtrace() returned %d addresses\n", nptrs);

    /* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
       would produce similar output to the following: */

    strings = backtrace_symbols(buffer, nptrs);
    if (strings == NULL)
    {
        perror("backtrace_symbols");
        exit(EXIT_FAILURE);
    }

    for (int j = 0; j < nptrs; j++)
        fprintf(stderr, "%s\n", strings[j]);

    free(strings);
}

/**
 * @brief Aborts the program and prints a backtrace.
 */
#define do_abort() \
    do { print_backtrace(); abort(); } while(0)

#define do_abortf(...) do_abortf_(__VA_ARGS__)
#define do_abortf_(fmt, ...) \
    do { fprintf(stderr, fmt "\n", ##__VA_ARGS__); print_backtrace(); abort(); } while(0)

/**
 * @brief Stringification macro.
 *
 * STR(x) expands macros in x and turn the result into a string
 */
#define STR(x) STR_(x)
#define STR_(x) #x

/**
 * @brief Logs entry to a (long-running) function
 *
 * @param kind  Boolean value that determines whether the log should be printed.
 * @param fmt   Format string for the function arguments.
 * @param ...   Arguments to be logged.
 */
#define log_proc(kind, ...) log_proc_(kind, __VA_ARGS__)
#define log_proc_(kind, fmt, ...) \
    do { if (kind) fprintf(stderr, "%s(" fmt ")\n", __func__, ##__VA_ARGS__); } while (0)

/**
 * @brief Logs entry to a leaf function
 *
 * @param kind  Boolean value that determines whether the log should be printed.
 * @param fmt   Format string for the function arguments.
 * @param ...   Arguments to be logged.
 */
#define log_func(kind, ...) log_func_(kind, __VA_ARGS__)
#define log_func_(kind, fmt, ...) \
    do { if (kind) fprintf(stderr, "%s(" fmt ")", __func__, ##__VA_ARGS__); } while(0)

/**
 * @brief Logs function result with a format string.
 *
 * @param kind  Boolean value that determines whether the log should be printed.
 * @param fmt   Format string for the result.
 * @param ...   Result to be logged.
 */
#define log_result(kind, ...) log_result_(kind, __VA_ARGS__)
#define log_result_(kind, fmt, ...) \
    do { if (kind) fprintf(stderr, " = " fmt "\n", ##__VA_ARGS__); } while(0)

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

// File format management
//
// It is common to omit file extensions in TeX ecosystem :)
// ... We need to guess them.

const char *exts_enc[]       = {".enc", NULL};
const char *exts_font_map[]  = {".map", NULL};
const char *exts_tfm[]       = {".tfm", NULL};
const char *exts_vf[]        = {".vf", NULL};
const char *exts_true_type[] = {".ttf", NULL};
const char *exts_type1[]     = {".pfb", NULL};
const char *exts_open_type[] = {".otf", NULL};
const char *exts_tex[]       = {".tex", NULL};
const char *exts_none[]      = {NULL};

const char **format_extensions(ttbc_file_format format)
{
    switch (format)
    {
        case TTBC_FILE_FORMAT_ENC:       return exts_enc;
        case TTBC_FILE_FORMAT_FONT_MAP:  return exts_font_map;
        case TTBC_FILE_FORMAT_TFM:       return exts_tfm;
        case TTBC_FILE_FORMAT_VF:        return exts_vf;
        case TTBC_FILE_FORMAT_TRUE_TYPE: return exts_true_type;
        case TTBC_FILE_FORMAT_TYPE1:     return exts_type1;
        case TTBC_FILE_FORMAT_OPEN_TYPE: return exts_open_type;
        case TTBC_FILE_FORMAT_TEX:       return exts_tex;
        default:                         return exts_none;
    }
}

#define CASE(x) case TTBC_FILE_FORMAT_##x: return #x

static const char *ttbc_file_format_to_string(ttbc_file_format format)
{
    switch (format)
    {
        CASE(AFM);
        CASE(BIB);
        CASE(BST);
        CASE(CMAP);
        CASE(CNF);
        CASE(ENC);
        CASE(FORMAT);
        CASE(FONT_MAP);
        CASE(MISC_FONTS);
        CASE(OFM);
        CASE(OPEN_TYPE);
        CASE(OVF);
        CASE(PICT);
        CASE(PK);
        CASE(PROGRAM_DATA);
        CASE(SFD);
        CASE(TECTONIC_PRIMARY);
        CASE(TEX);
        CASE(TEX_PS_HEADER);
        CASE(TFM);
        CASE(TRUE_TYPE);
        CASE(TYPE1);
        CASE(VF);
    }
    static char buf[80];
    sprintf(buf, "unknown format %d", format);
    return buf;
}

// Input management
static FILE *input_as_file(ttbc_input_handle_t *h) { return (void*)h; }
static ttbc_input_handle_t *file_as_input(FILE *h) { return (void*)h; }

ttbc_input_handle_t *ttstub_input_open(char const *path, ttbc_file_format format, int is_gz)
{
    log_proc(logging, "path:%s, format:%s, is_gz:%b", path, ttbc_file_format_to_string(format), is_gz);
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
        fprintf(stderr, "input_open: failed to open file %s\n", path);

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
    log_proc(logging, "path:%s, is_gz:%b", path, is_gz);
    if (is_gz)
        do_abortf("is_gz not supported");
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
    in_initex_mode = true;
    primary_document = "xelatex.ini";
    tt_history_t result = tt_run_engine("xetex.fmt", "xelatex.ini", EXECUTION_DATE);

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
