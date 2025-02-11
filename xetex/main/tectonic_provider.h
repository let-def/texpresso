#ifndef TECTONIC_PROVIDER_H_
#define TECTONIC_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

/**
 * Check if Tectonic provider is available.
 * @return true if Tectonic is available, false otherwise.
 */
bool tectonic_available(void);

/**
 * Check if a file with the given name is available in Tectonic.
 * @param name The name of the file to check.
 * @return true if the file is available, false otherwise.
 */
bool tectonic_has_file(const char *name);

/**
 * Get a file with the given name from Tectonic.
 * @param name The name of the file to get.
 * @return A FILE pointer to the file, or NULL if the file is not found.
 */
FILE *tectonic_get_file(const char *name);

/**
 * Record the version of Tectonic (as a hash).
 * @param f The file pointer where the version will be recorded.
 */
void tectonic_record_version(FILE *f);

/**
 * Check if the version recorded in the file matches the current Tectonic version.
 * @param f The file pointer containing the version to check.
 * @return true if the versions match, false otherwise.
 */
bool tectonic_check_version(FILE *f);

#endif  // TECTONIC_PROVIDER_H_
