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

/**
 * @file engine_pdf.c
 * @brief PDF display engine implementation
 *
 * This module implements a simple PDF engine for displaying pre-compiled
 * PDF documents. Unlike the TeX engine, it doesn't support incremental
 * compilation - it only displays existing PDF files.
 *
 * @par Architecture
 *
 * The PDF engine uses MuPDF's built-in PDF rendering capabilities:
 * - Opens and loads PDF document on creation
 * - Renders pages to display lists on demand
 * - Supports file change detection via re-opening
 *
 * @par Features
 *
 * - Read-only document viewing
 * - Automatic file change monitoring
 * - Support for standard PDF features (annotations, forms, etc.)
 * - No incremental compilation support
 *
 * @par Comparison with TeX Engine
 *
 * Unlike the TeX engine:
 * - No child process management
 * - No incremental compilation or rollback
 * - No filesystem tracking
 * - No Synctex support
 * - Always terminates after initial load
 *
 * @see engine.h Common engine interface
 * @see engine_tex.c TeX incremental compilation engine
 * @see engine_dvi.c DVI display engine
 */

#include <mupdf/fitz.h>
#include "engine.h"

/**
 * @brief PDF engine instance structure
 *
 * Simple engine for displaying pre-compiled PDF documents.
 * Uses MuPDF's built-in PDF rendering capabilities.
 *
 * @see Engine Base engine structure
 * @see EngineClass Virtual method table
 */
struct PdfEngine
{
  struct EngineClass *_class; /**< Virtual method table */
  char *path;                 /**< Path to the PDF file */
  int page_count;             /**< Number of pages in document */
  fz_document *doc;           /**< MuPDF document handle */
  bool changed;               /**< Document has been reloaded */
};

#define SELF struct PdfEngine *self = (struct PdfEngine*)_self

// Useful routines

TXP_ENGINE_DEF_CLASS;

/**
 * @brief Destroy the PDF engine and free resources
 *
 * Closes the PDF document and frees allocated memory.
 * This is the implementation of EngineClass->destroy for PDF engine.
 *
 * @param _self Engine instance (cast to PdfEngine)
 * @param ctx   MuPDF context for resource deallocation
 */
static void engine_destroy(Engine *_self, fz_context *ctx)
{
  SELF;
  fz_free(ctx, self->path);
  fz_drop_document(ctx, self->doc);
}

/**
 * @brief Render a page to a display list
 *
 * Loads the specified page from the PDF document and creates
 * a display list that can be rendered multiple times.
 *
 * @param _self Engine instance (cast to PdfEngine)
 * @param ctx   MuPDF context for rendering
 * @param index Page number (0-indexed)
 * @return Display list containing the page content, or NULL on error
 */
static fz_display_list *engine_render_page(Engine *_self,
                                           fz_context *ctx,
                                           int index)
{
  SELF;
  fz_page *page = fz_load_page(ctx, self->doc, index);
  fz_display_list *dl = fz_new_display_list_from_page(ctx, page);
  fz_drop_page(ctx, page);
  return dl;
}

/**
 * @brief Step the engine (no-op for PDF)
 *
 * PDF engine doesn't have a child process, so this always returns false.
 *
 * @param _self              Engine instance
 * @param ctx               MuPDF context
 * @param restart_if_needed Ignored (no process to restart)
 * @return false (no processing done)
 */
static bool engine_step(Engine *_self,
                        fz_context *ctx,
                        bool restart_if_needed)
{
  return 0;
}

/**
 * @brief Begin a transaction (no-op for PDF)
 *
 * PDF engine doesn't track source files, so this does nothing.
 * The file is re-opened in detect_changes() instead.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 */
static void engine_begin_changes(Engine *_self, fz_context *ctx)
{
}

/**
 * @brief Detect changes by re-opening the PDF file
 *
 * Re-opens the PDF file and replaces the document handle if successful.
 * Sets the changed flag to indicate the document has been reloaded.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 */
static void engine_detect_changes(Engine *_self, fz_context *ctx)
{
  SELF;
  fz_document *doc = fz_open_document(ctx, self->path);
  if (!doc)
    return;

  fz_drop_document(ctx, self->doc);
  self->doc = doc;
  self->page_count = fz_count_pages(ctx, doc);
  self->changed = 1;
}

/**
 * @brief End a transaction for PDF
 *
 * Returns true if the document was reloaded (changed flag was set),
 * false otherwise.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 * @return true if document was reloaded, false otherwise
 */
static bool engine_end_changes(Engine *_self, fz_context *ctx)
{
  SELF;
  if (self->changed)
  {
    self->changed = 0;
    return 1;
  }
  else
  return 0;
}

/**
 * @brief Get the number of pages in the PDF document
 *
 * @param _self Engine instance
 * @return Number of pages in the PDF document
 */
static int engine_page_count(Engine *_self)
{
  SELF;
  return self->page_count;
}

/**
 * @brief Get the engine status (always terminated for PDF)
 *
 * PDF engines don't run child processes, so they always return
 * DOC_TERMINATED.
 *
 * @param _self Engine instance
 * @return DOC_TERMINATED
 */
static EngineStatus engine_get_status(Engine *_self)
{
  return DOC_TERMINATED;
}

/**
 * @brief Get the scaling factor for PDF rendering
 *
 * PDF documents use a 1:1 scaling factor (device pixels match
 * PDF points).
 *
 * @param _self Engine instance
 * @return 1.0 (PDF uses points as units)
 */
static float engine_scale_factor(Engine *_self)
{
  return 1;
}

/**
 * @brief Get Synctex data (not available for PDF)
 *
 * PDF engines don't support Synctex synchronization.
 *
 * @param _self Engine instance
 * @param buf   If non-NULL, set to NULL
 * @return NULL
 */
static TexSynctex *engine_synctex(Engine *_self, fz_buffer **buf)
{
  return NULL;
}

/**
 * @brief Find a file (not available for PDF)
 *
 * PDF engines don't track source files.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 * @param path  File path (unused)
 * @return NULL
 */
static FileEntry *engine_find_file(Engine *_self, fz_context *ctx, const char *path)
{
  return NULL;
}

/**
 * @brief Notify file changes (not available for PDF)
 *
 * PDF engines don't track source files. Changes are detected
 * by re-opening the file in detect_changes().
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 * @param entry File entry (unused)
 * @param offset Change offset (unused)
 */
static void engine_notify_file_changes(Engine *_self,
                                       fz_context *ctx,
                                       FileEntry *entry,
                                       int offset)
{
}

/**
 * @brief Create a new PDF engine from a file
 *
 * Opens and loads the PDF document, counts pages, and creates
 * the engine instance.
 *
 * @param ctx      MuPDF context for memory allocation
 * @param pdf_path Path to the PDF file to display
 * @return Engine instance, or NULL if file cannot be opened
 */
Engine *create_pdf_engine(fz_context *ctx, const char *pdf_path)
{
  fz_document *doc = fz_open_document(ctx, pdf_path);
  if (!doc)
    return NULL;

  struct PdfEngine *self = fz_malloc_struct(ctx, struct PdfEngine);
  self->_class = &_class;

  self->path = fz_strdup(ctx, pdf_path);
  self->doc = doc;
  self->page_count = fz_count_pages(ctx, doc);

  return (Engine*)self;
}
