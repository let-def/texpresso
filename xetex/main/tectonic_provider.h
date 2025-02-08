#ifndef TECTONIC_PROVIDER_H_
#define TECTONIC_PROVIDER_H_

#include <stdio.h>
#include "tectonic_bridge_core.h"

bool tectonic_has_file(const char *name);
FILE *tectonic_get_file(const char *name);
void tectonic_close_file(FILE *f);

#endif  // TECTONIC_PROVIDER_H_
