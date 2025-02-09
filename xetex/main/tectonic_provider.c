#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include "tectonic_provider.h"
#include "utils.h"

int *entries;
int entries_pow = 1;

char *names = NULL;
size_t names_len = 0, names_cap = 1;

static void append_buffer(char *buffer, int len)
{
    size_t new_len = names_len + len;
    if (new_len > names_cap)
    {
        while (new_len > names_cap)
            names_cap *= 2;
        names = realloc(names, names_cap);
        if (names == NULL)
            do_abortf("Cannot allocate buffer to read Tectonic bundle");
    }

    memmove(names + names_len, buffer, len);
    names_len += len;
}

static unsigned long
sdbm_hash(const void *p)
{
  unsigned long hash = 0;
  int c;

  for (const unsigned char *str = p; (c = *str); str++)
    hash = c + (hash << 6) + (hash << 16) - hash;

  return hash * 2654435761;
}

int lookup_entry_index(const char *name)
{
    unsigned long hash = sdbm_hash(name);
    int mask = (1 << entries_pow) - 1;
    int index = hash & mask;

    while (1)
    {
        int offset = entries[index];
        if (offset == -1)
            return index;
        if (strcmp(name, names + offset) == 0)
            return index;
        index = (index + 1) & mask;
    }
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

static bool mkdir_p(char *path)
{
  for (char *p = path + 1; *p; p++)
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

const char *tectonic_cache_path(const char *basename)
{
  static char cache_path[PATH_MAX + 1];
  static int cache_dirlen = 0;

  if (cache_dirlen == 0)
  {
    const char *var;
    if ((var = getenv("XDG_CACHE_HOME")) && var[0])
    {
      strcpy(cache_path, var);
    }
    else if ((var = getenv("HOME")) && var[0])
    {
      strcpy(cache_path, var);
      strcat(cache_path, "/.cache");
    }
    else
    {
      cache_dirlen = -1;
      return NULL;
    }
    strcat(cache_path, "/texpresso/tectonic");
    normalize_path(cache_path);
    if (!mkdir_p(cache_path))
    {
      cache_dirlen = -1;
      return NULL;
    }
    cache_dirlen = strlen(cache_path);
  }

  if (cache_dirlen < 0)
    return NULL;

  if (basename)
  {
    cache_path[cache_dirlen] = '/';
    int i = cache_dirlen + 1, j;
    for (j = i; j < PATH_MAX && basename[j - i]; j++)
      cache_path[j] = basename[j - i];
    cache_path[j] = '\0';
  }
  else
    cache_path[cache_dirlen] = 0;
  return cache_path;
}

static bool check_cache_validity(char checksum[4096])
{
  const char *dir = tectonic_cache_path("SHA256SUM");

  // Give up if there is no cache directory
  if (!dir)
    return 0;

  FILE *f;

  // Check version of our cache
  f = fopen(dir, "rb");
  // No SHA256SUM: cache is either uninitialized or damaged
  if (!f)
    return 0;

  char b1[4096];
  int r1 = fread(b1, 1, 4096, f);
  fclose(f);

  // Compare tectonic SHA256SUM and ours
  FILE *p = popen("tectonic -X bundle cat SHA256SUM", "r");
  // No tectonic: we cannot check cache :/
  if (!p)
    return 0;

  int r2 = fread(checksum, 1, 4096, p);
  checksum[r2] = 0;
  pclose(p);

  return (r1 == r2) && (memcmp(b1, checksum, r1) == 0);
}

static void prepare_cache(void)
{
  char checksum[4096];
  if (check_cache_validity(checksum))
    return;

  // Base cache directory
  const char *path = tectonic_cache_path(NULL);

  DIR *dir;
  struct dirent *entry;
  char filePath[1024];

  if ((dir = opendir(path)) == NULL)
  {
    perror("opendir");
    return;
  }

  while ((entry = readdir(dir)) != NULL)
  {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    snprintf(filePath, sizeof(filePath), "%s/%s", path, entry->d_name);
    if (unlink(filePath) != 0)
      perror("unlink");
  }

  closedir(dir);

  const char *checksum_path = tectonic_cache_path("SHA256SUM");
  FILE *f = fopen(checksum_path, "wb");
  fwrite(checksum, 1, strlen(checksum), f);
  fclose(f);
}

static void load_tectonic_files(void)
{
  static int loaded = 0;
  if (loaded)
    return;
  loaded = 1;

  // Read the list of all files
  FILE *f = popen("tectonic -X bundle search", "r");
  if (f == NULL)
    do_abortf("Failed to execute 'tectonic -X bundle search'");

  char buffer[4096];
  ssize_t read;
  while ((read = fread(buffer, 1, 4096, f)) > 0)
  {
    append_buffer(buffer, read);
  }

  if (ferror(f))
  {
    perror("fread/popen");
  }

  pclose(f);

  // Count number of lines
  int count = 0;
  for (int i = 0; i < names_len; i++)
    if (names[i] == '\n')
      count += 1;

  // Allocate hashtable for indexing lines
  int min_cap = count * 4 / 3;
  while ((1 << entries_pow) <= min_cap)
    entries_pow++;
  entries = malloc((1 << entries_pow) * sizeof(*entries));
  if (entries == NULL)
    do_abortf("Cannot allocate table for indexing Tectonic bundle.");
  for (int i = 0; i < (1 << entries_pow); i++)
    entries[i] = -1;

  // Populate table
  for (int i = 0, j = 0; j < names_len; j++)
  {
    if (names[j] == '\n')
    {
      names[j] = '\0';
      int index = lookup_entry_index(names + i);
      if (entries[index] == -1)
        entries[index] = i;
      else
        fprintf(stderr, "tectonic bundle: duplicate entry (%s)\n", names + i);
      i = j + 1;
    }
  }

  fprintf(stderr, "tectonic bundle: indexing succeeded (%d entries)\n", count);
  prepare_cache();
}

FILE *tectonic_cat(const char *name)
{
  if (!tectonic_has_file(name))
    return NULL;
  char buffer[1024] = "tectonic -X bundle cat ", *p = buffer;
  while (*p)
    p++;

  *p++ = '\'';
  while (*name)
  {
    if ((*p++ = *name++) != '\'')
      continue;
    *p++ = '\\';
    *p++ = '\'';
    *p++ = '\'';
  }
  *p++ = '\'';
  *p = '\0';

  return popen(buffer, "r");
}

bool tectonic_has_file(const char *name)
{
  load_tectonic_files();
  int index = lookup_entry_index(name);
  return (entries[index] != -1);
}

FILE *tectonic_get_file(const char *name)
{
  if (!tectonic_has_file(name))
    return NULL;

  const char *cached = tectonic_cache_path(name);
  FILE *f = fopen(cached, "rb");
  if (f)
    return f;
  f = fopen(cached, "wb");
  if (!f)
    return NULL;

  FILE *p = tectonic_cat(name);
  if (!p)
    return NULL;

  char buffer[4096];
  int read;
  while ((read = fread(buffer, 1, 4096, p)) > 0)
  {
    if (fwrite(buffer, 1, read, f) != read)
      break;
  }
  if (read != 0 || ferror(p) || ferror(f))
  {
    pclose(p); fclose(f);
    unlink(cached);
    return NULL;
  }
  pclose(p);
  fclose(f);

  return fopen(cached, "rb");
}
