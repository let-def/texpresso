#include <stdio.h>
#include <stdlib.h>
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
    char buffer[1024];
    int len = snprintf(buffer, 1023, "tectonic -X bundle cat %s", name);
    if (len >= 0)
    {
        buffer[len] = 0;
        return popen(buffer, "r");
    }
    return NULL;
}

void tectonic_close_file(FILE *f)
{
    pclose(f);
}
