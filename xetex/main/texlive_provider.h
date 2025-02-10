#ifndef TEXLIVE_PROVIDER_H_
#define TEXLIVE_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

const char *texlive_file_path(const char *name);
const char *texlive_cache_path(const char *basename);

#endif // TEXLIVE_PROVIDER_H_
