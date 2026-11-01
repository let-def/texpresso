/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef DVI_PDFSTORE_H
#define DVI_PDFSTORE_H

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdbool.h>

// Document-level store of the PDF objects defined by dvipdfmx specials
// (pdf:obj, pdf:put, pdf:stream, pdf:bxobj, pdf:image, ...).
//
// Objects are kept in a scratch MuPDF document, so that MuPDF can be used to
// load shadings, functions, colorspaces and images.
// Named objects ("@name" in specials) are mapped to indirect objects of this
// document. Form XObjects defined from DVI content (pdf:bxobj ... pdf:exobj)
// are dictionaries recording the range of DVI bytes to replay, using the
// private keys /TXPStart and /TXPEnd.

typedef struct dvi_pdfstore dvi_pdfstore;

dvi_pdfstore *dvi_pdfstore_new(fz_context *ctx);
void dvi_pdfstore_free(fz_context *ctx, dvi_pdfstore *ps);
pdf_document *dvi_pdfstore_doc(dvi_pdfstore *ps);

// The resource dictionary shared by all pages
pdf_obj *dvi_pdfstore_page_resources(dvi_pdfstore *ps);

// Object number associated to a name ("@name" without the "@").
// If the name is unknown, returns 0 or allocate a new (null) object when
// `create` is set.
int dvi_pdfstore_name(fz_context *ctx, dvi_pdfstore *ps, const char *name, int len, bool create);

// Open a stream on [cur, lim), after resolving the "@name" references
// of dvipdfmx syntax.
fz_stream *dvi_pdfstore_open(fz_context *ctx, dvi_pdfstore *ps,
                             const char *cur, const char *lim);

// Parse the next object from a stream returned by dvi_pdfstore_open.
// Returns NULL at the end of the stream.
pdf_obj *dvi_pdfstore_parse(fz_context *ctx, dvi_pdfstore *ps, fz_stream *stm);

// Forget the forms defined from DVI bytes located after `offset`.
void dvi_pdfstore_invalidate(fz_context *ctx, dvi_pdfstore *ps, size_t offset);

#endif /*!DVI_PDFSTORE_H*/
