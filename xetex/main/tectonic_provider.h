#ifndef TECTONIC_PROVIDER_H_
#define TECTONIC_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

bool tectonic_has_file(const char *name);
FILE *tectonic_get_file(const char *name);
void tectonic_close_file(FILE *f);
const char *tectonic_cache_path(const char *basename);

#endif  // TECTONIC_PROVIDER_H_
