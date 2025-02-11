#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "utils.h"

int logging = 1;
#define BT_BUF_SIZE 100

/**
 * @brief Prints a backtrace to stderr.
 *
 * This function captures the current call stack and prints it to the standard error output.
 * It uses the `backtrace` and `backtrace_symbols` functions to retrieve and format the stack trace.
 */
void print_backtrace(void)
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

static void normalize_path(char *path0)
{
  char *index = path0, *path = path0;
  while (*path)
  {
    *index = *path;
    if (*path == '/')
      while (path[1] == '/')
        path++;
    index++;
    path++;
  }
  while (index > path0 && index[-1] == '/')
    index--;
  *index = '\0';
}

static bool mkdir_path(char *path, char *base)
{
  for (char *p = base + 1; *p; p++)
  {
    if (*p != '/')
      continue;
    *p = '\0';
    bool fail = mkdir(path, S_IRWXU) != 0 && errno != EEXIST;
    *p = '/';
    if (fail)
    {
      perror("cache initialization: mkdir failed");
      return 0;
    }
  }
  bool fail = !(mkdir(path, S_IRWXU) != 0 && errno != EEXIST);
  if (fail)
  {
    perror("cache initialization: mkdir failed");
    return 0;
  }
  return 1;
}

const char *cache_path(const char *folder, const char *name)
{
  static char base[PATH_MAX + 1];
  static int baselen = 0;

  if (baselen == 0)
  {
    baselen = -1;

    const char *var;
    if ((var = getenv("XDG_CACHE_HOME")) && var[0])
    {
      if (snprintf(base, sizeof(base), "%s/texpresso", var) >= sizeof(base))
        return NULL;
    }
    else if ((var = getenv("HOME")) && var[0])
    {
      if (snprintf(base, sizeof(base), "%s/.cache/texpresso", var) >= sizeof(base))
        return NULL;
    }
    else
      return NULL;
    normalize_path(base);
    if (mkdir_path(base, base))
      baselen = strlen(base);
    else
      return NULL;
  }

  if (baselen < 0)
    return NULL;

  int len = baselen;
  if (folder && len < sizeof(base))
  {
    base[len++] = '/';
    len += snprintf(base + len, sizeof(base) - len, "%s", folder);
    mkdir_path(base, base + baselen + 1);
  }
  if (name && len < sizeof(base))
  {
    base[len++] = '/';
    len += snprintf(base + len, sizeof(base) - len, "%s", name);
  }

  if (len > PATH_MAX)
    abort();

  base[len] = 0;
  if (len == PATH_MAX)
  {
    fprintf(stderr, "Error: cache path is too long:\n%s\n", base);
    return NULL;
  }

  return base;
}
