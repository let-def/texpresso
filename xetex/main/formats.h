#ifndef FORMATS_H_
#define FORMATS_H_

#include "tectonic_bridge_core.h"

// File format management
//
// It is common to omit file extensions in TeX ecosystem :)
// ... We need to guess them.

// Return a null-terminated list of possible file extensions for "format"
const char **format_extensions(ttbc_file_format format);

// Format to string for debugging purposes
const char *ttbc_file_format_to_string(ttbc_file_format format);

#endif // FORMATS_H_
