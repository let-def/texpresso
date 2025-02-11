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

/**
 * Normalize a given file path by removing redundant slashes and trailing slashes.
 *
 * @param path0 The input path to be normalized. This path is modified in place.
 */
static void normalize_path(char *path0)
{
  char *index = path0, *path = path0;

  // Iterate over each character in the path
  while (*path)
  {
    *index = *path; // Copy the current character to the index position
    if (*path == '/')
      // Skip consecutive slashes
      while (path[1] == '/')
        path++;
    index++; // Move to the next position in the index
    path++;  // Move to the next character in the path
  }
  // Remove trailing slashes
  while (index > path0 && index[-1] == '/')
    index--;
  *index = '\0'; // Null-terminate the normalized path
}

/**
 * Recursively create directories in the given path.
 *
 * @param path The full path where directories are to be created.
 * @param base The base path to start creating directories from.
 *             base >= path && base <= path + strlen(base).
 * @return 1 on success, 0 on failure.
 */
static bool mkdir_path(char *path, char *base)
{
  // Iterate over base characters, starting from the second one
  for (char *p = base + 1; *p; p++)
  {
    // Skip characters that are not slashes
    if (*p != '/')
      continue;

    // Temporarily null-terminate the path at the current slash
    *p = '\0';

    // Attempt to create the directory and check if it exists or succeeds
    bool ok = mkdir(path, S_IRWXU) == 0 || errno == EEXIST;

    // Restore the slash character
    *p = '/';

    if (ok)
      continue;

    // Log error and return
    perror("cache initialization: mkdir failed");
    return 0;
  }

  // Attempt to create the final component of the path
  if (mkdir(path, S_IRWXU) == 0 || errno == EEXIST)
    return 1; // Success!

  // Log error and return
  perror("cache initialization: mkdir failed");
  return 0;
}

// The static buffer used by all calls to `cache_path`.
char cache_path_buffer[PATH_MAX + 1];

// Return the length of the base cache path, or -1 if initialization failed.
static int cache_base_init(void)
{
  // Compute user cache directory: $XDG_CACHE_HOME/texpresso or
  // $HOME/.cache/texpresso

  const char *var;
  // Check XDG_CACHE_HOME environment
  if ((var = getenv("XDG_CACHE_HOME")) && var[0])
  {
    // Append texpresso if possible
    if (snprintf(cache_path_buffer, sizeof(cache_path_buffer), "%s/texpresso",
                 var) >= sizeof(cache_path_buffer))
    {
      fprintf(stderr, "Error: overflow: $XDG_CACHE_HOME is too long.");
      return -1;
    }
  }
  // Check HOME environment
  else if ((var = getenv("HOME")) && var[0])
  {
    // Append .cache/texpresso if possible
    if (snprintf(cache_path_buffer, sizeof(cache_path_buffer),
                 "%s/.cache/texpresso", var) >= sizeof(cache_path_buffer))
    {
      fprintf(stderr, "Error: overflow: $HOME/.cache/texpresso is too long.");
      return -1;
    }
  }
  // Fail
  else
  {
    fprintf(stderr,
            "Error: cannot get cache directory, "
            "neither $XDG_CACHE_HOME nor $HOME are defined.");
    return -1;
  }

  // Normalize and create the path
  normalize_path(cache_path_buffer);
  if (!mkdir_path(cache_path_buffer, cache_path_buffer))
  {
    fprintf(stderr, "Error: cannot access cache directory: %s.",
            cache_path_buffer);
    return -1;
  }

  // Successful initialization, return length
  return strlen(cache_path_buffer);
}

/**
 * Construct a full cache path by normalizing and creating necessary directories.
 *
 * @param folder The subfolder within the cache path. Can be NULL.
 * @param name The file name within the cache path. Can be NULL.
 * @return A pointer to the constructed cache path on success, NULL on failure.
 */
const char *cache_path(const char *folder, const char *name)
{
  // 0  if uninitialized,
  // -1 if initialization failed,
  // >0 length of the base cache path if initialization succeeded
  //
  // Other components, if provided, are added after base path
  static int baselen = 0;

  // Initialize if necessary
  if (baselen == 0)
    baselen = cache_base_init();

  if (baselen < 0)
      return NULL;

  // Start appending after the base
  int len = baselen;

  // Append folder
  if (folder && *folder && len < sizeof(cache_path_buffer))
  {
    cache_path_buffer[len++] = '/';
    len += snprintf(cache_path_buffer + len, sizeof(cache_path_buffer) - len, "%s", folder);
    if (!mkdir_path(cache_path_buffer, cache_path_buffer + baselen + 1))
    {
      fprintf(stderr, "Error: cannot cache create directory %s\n",
              cache_path_buffer);
      return NULL;
    }
  }

  // Append name
  if (name && *name && len < sizeof(cache_path_buffer))
  {
    cache_path_buffer[len++] = '/';
    len += snprintf(cache_path_buffer + len, sizeof(cache_path_buffer) - len, "%s", name);
  }

  if (len > PATH_MAX)
    abort();

  // Signal error if path overflowed
  if (len == PATH_MAX)
  {
    fprintf(stderr, "Error: cache path is too long:\n%s\n", cache_path_buffer);
    return NULL;
  }

  return cache_path_buffer;
}
