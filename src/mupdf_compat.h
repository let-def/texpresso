#ifndef __MUPDF_COMPAT__
#define __MUPDF_COMPAT__

#include <stdlib.h>
#include <mupdf/fitz/version.h>

// fz_malloc_struct_array was introduced in mupdf 1.19.0

#if (FZ_VERSION_MAJOR == 1) && (FZ_VERSION_MINOR < 19)

#define fz_malloc_struct_array(CTX, N, TYPE) \
	((TYPE*)Memento_label(fz_calloc(CTX, N, sizeof(TYPE)), #TYPE "[]"))

#endif

#if (FZ_VERSION_MAJOR == 1) && (FZ_VERSION_MINOR == 16)

// Before mupdf 1.17.0, fz_buffer was defined as 
//   typedef struct fz_buffer_s fz_buffer;
// and fz_buffer_s was kept opaque.

struct fz_buffer_s
{
	int refs;
	unsigned char *data;
	size_t cap, len;
	int unused_bits;
	int shared;
};

#endif

#endif /*!__MUPDF_COMPAT__*/
