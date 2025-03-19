#ifndef __MUPDF_COMPAT__
#define __MUPDF_COMPAT__

#include <stdlib.h>
#include <mupdf/fitz/version.h>
#include <mupdf/fitz/geometry.h>
#include <mupdf/fitz/stream.h>

// fz_malloc_struct_array, fz_irect_width and fz_irect_height
// were introduced in mupdf 1.19.0

#if (FZ_VERSION_MAJOR == 1) && (FZ_VERSION_MINOR < 19)

#define fz_malloc_struct_array(CTX, N, TYPE) \
	((TYPE*)Memento_label(fz_calloc(CTX, N, sizeof(TYPE)), #TYPE "[]"))

static inline unsigned int
fz_irect_width(fz_irect r)
{
	unsigned int w;
	if (r.x0 >= r.x1)
		return 0;
	return (int)(r.x1 - r.x0);
}

static inline unsigned int
fz_irect_height(fz_irect r)
{
	unsigned int w;
	if (r.y0 >= r.y1)
		return 0;
	return (int)(r.y1 - r.y0);
}

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

// fz_open_file_ptr_no_close is implemented but not exposed by mupdf 1.16
fz_stream *fz_open_file_ptr_no_close(fz_context *ctx, FILE *file);

#endif

#endif /*!__MUPDF_COMPAT__*/
