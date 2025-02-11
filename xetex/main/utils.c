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
      return 0;
  }
  return !(mkdir(path, S_IRWXU) != 0 && errno != EEXIST);
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
      strcpy(base, var);
      strcat(base, "/texpresso");
    }
    else if ((var = getenv("HOME")) && var[0])
    {
      strcpy(base, var);
      strcat(base, "/.cache/texpresso");
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
  if (folder)
  {
    base[len++] = '/';
    for (int i = len; len < PATH_MAX && folder[len - i]; len++)
      base[len] = folder[len - i];
    base[len] = 0;
    mkdir_path(base, base + baselen + 1);
  }
  if (name)
  {
    base[len++] = '/';
    for (int i = len; len < PATH_MAX && name[len - i]; len++)
      base[len] = name[len - i];
  }

  if (len >= PATH_MAX)
      return NULL;

  base[len] = 0;
  return base;
}
