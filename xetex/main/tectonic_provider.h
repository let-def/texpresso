#ifndef TECTONIC_PROVIDER_H_
#define TECTONIC_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

bool tectonic_has_file(const char *name);
FILE *tectonic_get_file(const char *name);
const char *tectonic_cache_path(const char *basename);

void tectonic_record_version(FILE *f);
bool tectonic_check_version(FILE *f);

#endif  // TECTONIC_PROVIDER_H_
