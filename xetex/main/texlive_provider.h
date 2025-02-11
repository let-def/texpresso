#ifndef TEXLIVE_PROVIDER_H_
#define TEXLIVE_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

const char *texlive_file_path(const char *name, FILE *record_dependency);
bool texlive_check_dependencies(FILE *record);

#endif // TEXLIVE_PROVIDER_H_
