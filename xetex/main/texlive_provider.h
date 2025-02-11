#ifndef TEXLIVE_PROVIDER_H_
#define TEXLIVE_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

/**
 * Check if TeX Live is available on the system.
 *
 * @return true if TeX Live is available, false otherwise.
 */
bool texlive_available(void);

/**
 * Retrieve the file path for a given TeX Live file.
 *
 * @param name The name of the TeX Live file to locate.
 * @param record_dependency A writable file stream to record the dependency, or NULL.
 * @return The file path of the TeX Live file, or NULL if not found.
 */
const char *texlive_file_path(const char *name, FILE *record_dependency);

/**
 * Check if all recorded dependencies for TeX Live are still up to date.
 *
 * @param record A file stream to read the dependencies.
 * @return true if dependencies are still up, false otherwise.
 */
bool texlive_check_dependencies(FILE *record);

#endif // TEXLIVE_PROVIDER_H_
