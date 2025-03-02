#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "formats.h"
#include "tectonic_bridge_core.h"
#include "tectonic_bridge_core_generated.h"
#include "tectonic_provider.h"
#include "texlive_provider.h"
#include "utils.h"

#define LOG 0

/**
 * Global definitions
 */

// Name of root document
const char *primary_document = NULL;

// Path of the last opened file
char last_open[PATH_MAX + 1];

// Use tectonic provider
bool use_tectonic = 0;

// Use texlive provider
bool use_texlive = 0;

// A file in which dependencies are recorded (or NULL if not necessary)
FILE *dependency_tape;

// Name of the format to use
const char *format_name;

static const char *format_path(const char *ext);

// Buffer for printing format string
#define FORMAT_BUF_SIZE 4096
char format_buf[FORMAT_BUF_SIZE];

NORETURN PRINTF_FUNC(1, 2) int _tt_abort(const char *format, ...)
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
  // FIXME: input is UTF-16
  log_proc(logging, "%.*s, %d", (int)len, (char *)cmd, (int)len);
  return 0;
}

// Input management
static FILE *input_as_file(ttbc_input_handle_t *h)
{
  return (void *)h;
}
static ttbc_input_handle_t *file_as_input(FILE *h)
{
  return (void *)h;
}

ttbc_input_handle_t *ttstub_input_open(const char *path,
                                       ttbc_file_format format,
                                       int is_gz)
{
  log_proc(logging, "path:%s, format:%s, is_gz:%d", path,
           ttbc_file_format_to_string(format), is_gz);

  if (is_gz != 0)
    do_abortf("GZ compression not supported");

  FILE *f = fopen(path, "rb");
  const void *buffer = NULL;
  size_t len = 0;
  char tmp[PATH_MAX + 1];

  if (!f && format == TTBC_FILE_FORMAT_FORMAT)
  {
    const char *cached = format_path(".fmt");
    if (cached)
      f = fopen(cached, "rb");
  }

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
    if (LOG)
    {
      fprintf(stderr, "input_open: failed to open file %s\n", path);
      fprintf(stderr, "Trying texlive provider.\n");
    }

    if (use_texlive)
    {
      const char *tlpath = texlive_file_path(path, dependency_tape);
      for (const char **exts = format_extensions(format); !tlpath && *exts;
           exts++)
      {
        strcpy(tmp, path);
        strcat(tmp, *exts);
        tlpath = texlive_file_path(tmp, NULL);
      }
      if (tlpath)
      {
        strcpy(last_open, tlpath);
        f = fopen(tlpath, "rb");
      }
      else if (LOG)
        fprintf(stderr, "Texlive failed to provide the file.\n");
    }
    else
    {
      f = tectonic_get_file(path);
      if (f)
        strcpy(last_open, path);
      else
      {
        for (const char **exts = format_extensions(format); !f && *exts; exts++)
        {
          strcpy(tmp, path);
          strcat(tmp, *exts);
          f = tectonic_get_file(tmp);
        }
        if (f)
          strcpy(last_open, tmp);
      }
      if (!f && LOG)
        fprintf(stderr, "Texlive failed to provide the file.\n");
    }
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
  if (fstat(fileno(input_as_file(handle)), &file_stat) == -1)
  {
    return 0;  // Error getting file statistics
  }
  return file_stat.st_mtime;
}

size_t ttstub_input_get_size(ttbc_input_handle_t *handle)
{
  struct stat file_stat;
  if (fstat(fileno(input_as_file(handle)), &file_stat) == -1)
  {
    return 0;  // Error getting file statistics
  }
  return file_stat.st_size;
}

size_t ttstub_input_seek(ttbc_input_handle_t *handle,
                         ssize_t offset,
                         int whence)
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
static FILE *output_as_file(ttbc_output_handle_t *h)
{
  return (void *)h;
}
static ttbc_output_handle_t *file_as_output(FILE *h)
{
  return (void *)h;
}

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
  path = format_path(".fmt");
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

size_t ttstub_output_write(ttbc_output_handle_t *handle,
                           const char *data,
                           size_t len)
{
  return fwrite(data, 1, len, output_as_file(handle));
}

PRINTF_FUNC(2, 3)
int ttstub_fprintf(ttbc_output_handle_t *handle, const char *format, ...)
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
  return (void *)(++curr_diag);
}

ttbc_diagnostic_t *ttbc_diag_begin_warning(void)
{
  if (diag_kind != DIAG_NONE)
    do_abortf("Unexpected nested diagnostics");
  diag_kind = DIAG_WARN;
  diag_len = 0;
  return (void *)(++curr_diag);
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

static void diag_vprintf(ttbc_diagnostic_t *diag,
                         const char *format,
                         va_list ap)
{
  diag_write(diag, format_buf,
             vsnprintf(format_buf, FORMAT_BUF_SIZE, format, ap));
}

PRINTF_FUNC(2, 3)
void ttstub_diag_printf(ttbc_diagnostic_t *diag, const char *format, ...)
{
  log_proc(logging, "\"%s\", ...", format);
  va_list ap;
  va_start(ap, format);
  diag_vprintf(diag, format, ap);
  va_end(ap);
}

// Error helpers
PRINTF_FUNC(1, 2) void ttstub_issue_warning(const char *format, ...)
{
  ttbc_diagnostic_t *diag = ttbc_diag_begin_warning();
  va_list ap;
  va_start(ap, format);
  diag_vprintf(diag, format, ap);
  va_end(ap);
  ttstub_diag_finish(diag);
}

PRINTF_FUNC(1, 2) void ttstub_issue_error(const char *format, ...)
{
  ttbc_diagnostic_t *diag = ttbc_diag_begin_error();
  va_list ap;
  va_start(ap, format);
  diag_vprintf(diag, format, ap);
  va_end(ap);
  ttstub_diag_finish(diag);
}

// FIXME: Implement bounds caching later

int ttstub_pic_get_cached_bounds(const char *name, int type, int page, float bounds[4])
{
  return 0;
}

void ttstub_pic_set_cached_bounds(const char *name, int type, int page, const float bounds[4])
{
}

int texpresso_fork_with_channel(int fd, uint32_t time)
{

}

// Entry point

static void usage(char *argv0)
{
  fprintf(
      stderr,
      "Usage: %s [-texlive] [-tectonic] <path.tex>\n"
      "Run XeTeX engine on <path.tex> using packages from a TeX distribution.\n"
      "\n"
      "Supported TeX distributions:\n"
      "  -texlive     Use TeXlive packages (need kpsewhich command)\n"
      "  -tectonic    Use Tectonic packages (need tectonic command)\n"
      "Default: try TeXlive first, then Tectonic, then fails\n",
      argv0);
}

// Generate everything based on 1738978143
// (February 8, 2025)
#define EXECUTION_DATE 1738978143

static const char *format_path(const char *ext)
{
  const char *prefix = use_texlive ? "texlive-" : "tectonic-";
  return cache_path("format", prefix, format_name, ext);
}

static bool validate_format(void)
{
  const char *path;

  path = format_path(".fmt");
  if (!path || access(path, R_OK) != 0)
    return 0;

  path = format_path(".deps");
  if (!path)
    return 0;

  FILE *tape = fopen(path, "rb");
  if (!tape)
    return 0;

  bool result;
  if (use_texlive)
    result = texlive_check_dependencies(tape);
  else
    result = tectonic_check_version(tape);

  fclose(tape);

  return result;
}

static bool bootstrap_format(void)
{
  const char *path = format_path(".deps");
  if (!path)
    return 0;

  dependency_tape = fopen(path, "wb");
  if (!dependency_tape)
    return 0;

  if (use_tectonic)
  {
    tectonic_record_version(dependency_tape);
    fclose(dependency_tape);
    dependency_tape = NULL;
  }

  in_initex_mode = true;
  primary_document = format_name;
  tt_history_t result =
      tt_run_engine("texpresso.fmt", format_name, EXECUTION_DATE);
  in_initex_mode = false;
  primary_document = NULL;

  if (dependency_tape)
    fclose(dependency_tape);

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
    unlink(format_path(".fmt"));

  return (result == HISTORY_SPOTLESS);
}

int main(int argc, char **argv)
{
  const char *doc_path = NULL;
  bool dashdash = 0;

  for (int i = 1; i < argc; i++)
  {
    if (!dashdash && argv[i][0] == '-')
    {
      if (strcmp(argv[i], "-tectonic") == 0)
        use_tectonic = 1;
      else if (strcmp(argv[i], "-texlive") == 0)
        use_texlive = 1;
      else if (strcmp(argv[i], "--") == 0)
        dashdash = 1;
      else
      {
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
      }
    }
    else if (doc_path == NULL)
      doc_path = argv[i];
    else
    {
      fprintf(stderr,
              "Extraneous argument: %s\n"
              "Only one input file can be provided\n",
              argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  if (use_tectonic && use_texlive)
  {
    fprintf(stderr, "Provide either -tectonic or -texlive.\n");
    usage(argv[0]);
    return 1;
  }
  else if (use_texlive)
  {
    if (!texlive_available())
    {
      fprintf(stderr,
              "Cannot find TeXlive "
              "(ensure kpsewhich command is executable).\n");
      usage(argv[0]);
      return 1;
    }
  }
  else if (use_tectonic)
  {
    if (!tectonic_available())
    {
      fprintf(stderr,
              "Cannot find Tectonic "
              "(ensure tectonic command is executable).\n");
      usage(argv[0]);
      return 1;
    }
  }
  else if (texlive_available())
    use_texlive = 1;
  else if (tectonic_available())
    use_tectonic = 1;
  else
  {
    fprintf(stderr,
            "Neither TeXlive (via kpsewhich command) nor "
            "Tectonic (via tectonic command) are available.\n"
            "Cannot run without a TeX distribution.\n"
            "\n");
    usage(argv[0]);
    return 1;
  }

  if (use_texlive)
    fprintf(stderr, "Using TeXlive.\n");
  else
    fprintf(stderr, "Using Tectonic.\n");

  // Check if format file needs to be generated
  format_name = "xelatex.ini";
  if (!validate_format() && !bootstrap_format())
  {
    fprintf(stderr, "Failed to generate format.\n");
    return 1;
  }

  // Generate document
  if (!doc_path)
  {
    fprintf(stderr, "Expecting a .tex input file.\n");
    usage(argv[0]);
    return 1;
  }

  in_initex_mode = false;
  primary_document = doc_path;
  tt_history_t result =
      tt_run_engine("texpresso.fmt", primary_document, EXECUTION_DATE);

  fprintf(stderr, "Document generation: ");
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
