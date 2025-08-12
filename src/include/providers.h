#ifndef TECTONIC_PROVIDER_H_
#define TECTONIC_PROVIDER_H_

#include <stdio.h>
#include <stdbool.h>

/************/
/* TECTONIC */
/************/

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
 * Get the path to a cached copy of a Tectonic file.
 * @param name The name of the file to get.
 * @return The path to the file, or NULL if the file is not found or not
 *         available.
 */
const char *tectonic_get_file_path(const char *name);

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

/***********/
/* TEXLIVE */
/***********/

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

/********************/
/* CACHE MANAGEMENT */
/********************/

/**
 * Construct a cache path based on the provided folder and name.
 *
 * This function constructs a cache path by combining the folder and name with
 * a base path derived from the XDG_CACHE_HOME or HOME environment variables.
 * It ensures that the directory structure exists and is properly normalized.
 *
 * @param folder The subdirectory name within the cache path.
 * @param name The file name within the cache path.
 * @return The constructed cache path, or NULL if an error occurs.
 *         The returned buffer is managed by the function and valid until the
 *         next call.
 */
const char *cache_path_(const char *folder, const char *name[]);
#define cache_path(folder, ...) cache_path_(folder, (const char*[]){__VA_ARGS__, NULL})

#endif  // TECTONIC_PROVIDER_H_
