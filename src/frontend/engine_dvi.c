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
 * @file engine_dvi.c
 * @brief DVI display engine implementation
 *
 * This module implements a simple DVI engine for displaying pre-compiled
 * DVI documents. Unlike the TeX engine, it doesn't support incremental
 * compilation - it only displays existing DVI files.
 *
 * @par Architecture
 *
 * The DVI engine uses MuPDF's DVI rendering capabilities:
 * - Reads DVI file into buffer on creation
 * - Initializes internal DVI parser
 * - Renders pages to display lists on demand
 * - Supports file change detection via re-reading
 *
 * @par Features
 *
 * - Read-only document viewing
 * - Automatic file change monitoring
 * - Supports DVI format from TeX compilers
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
 * @see engine_pdf.c PDF display engine
 */

#include <mupdf/fitz.h>
#include "engine.h"
#include "incdvi.h"

/**
 * @brief DVI engine instance structure
 *
 * Simple engine for displaying pre-compiled DVI documents.
 * Uses MuPDF's internal DVI rendering capabilities.
 *
 * @see Engine Base engine structure
 * @see EngineClass Virtual method table
 */
struct DviEngine
{
  struct EngineClass *_class; /**< Virtual method table */
  fz_buffer *buffer;          /**< Raw DVI file data */
  IncDVI *dvi;                /**< Internal DVI parser state */
};

#define SELF struct DviEngine *self = (struct DviEngine*)_self

// Useful routines

TXP_ENGINE_DEF_CLASS;

/**
 * @brief Destroy the DVI engine and free resources
 *
 * Frees the DVI buffer and the internal DVI parser.
 * This is the implementation of EngineClass->destroy for DVI engine.
 *
 * @param _self Engine instance (cast to DviEngine)
 * @param ctx   MuPDF context for resource deallocation
 */
static void engine_destroy(Engine *_self, fz_context *ctx)
{
  SELF;
  fz_drop_buffer(ctx, self->buffer);
  incdvi_free(ctx, self->dvi);
}

/**
 * @brief Render a page to a display list
 *
 * Extracts page dimensions from the DVI data and renders the page
 * into a display list using the internal DVI parser.
 *
 * @param _self Engine instance (cast to DviEngine)
 * @param ctx   MuPDF context for rendering
 * @param index Page number (0-indexed)
 * @return Display list containing the page content, or NULL on error
 */
static fz_display_list *engine_render_page(Engine *_self,
                                           fz_context *ctx,
                                           int index)
{
  SELF;
  float width, height;
  incdvi_page_dim(self->dvi, self->buffer, index, &width, &height, NULL);
  fz_rect box = fz_make_rect(0, 0, width, height);
  fz_display_list *dl = fz_new_display_list(ctx, box);
  fz_device *dev = fz_new_list_device(ctx, dl);
  incdvi_render_page(ctx, self->dvi, self->buffer, index, dev);
  fz_close_device(ctx, dev);
  fz_drop_device(ctx, dev);
  return dl;
}

/**
 * @brief Step the engine (no-op for DVI)
 *
 * DVI engine doesn't have a child process, so this always returns false.
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
 * @brief Begin a transaction (no-op for DVI)
 *
 * DVI engine doesn't track source files, so this does nothing.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 */
static void engine_begin_changes(Engine *_self, fz_context *ctx)
{
}

/**
 * @brief Detect changes (no-op for DVI)
 *
 * DVI engine doesn't track source files, so this does nothing.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 */
static void engine_detect_changes(Engine *_self, fz_context *ctx)
{
}

/**
 * @brief End a transaction (no-op for DVI)
 *
 * DVI engine doesn't support incremental changes, so this always
 * returns false indicating no changes were applied.
 *
 * @param _self Engine instance
 * @param ctx   MuPDF context
 * @return false (no changes)
 */
static bool engine_end_changes(Engine *_self, fz_context *ctx)
{
  return 0;
}


/**
 * @brief Get the number of pages in the DVI document
 *
 * @param _self Engine instance
 * @return Number of pages in the DVI document
 */
static int engine_page_count(Engine *_self)
{
  SELF;
  return incdvi_page_count(self->dvi);
}

/**
 * @brief Get the engine status (always terminated for DVI)
 *
 * DVI engines don't run child processes, so they always return
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
 * @brief Get the scaling factor for DVI rendering
 *
 * @param _self Engine instance
 * @return Scaling factor from the internal DVI parser
 */
static float engine_scale_factor(Engine *_self)
{
  SELF;
  return incdvi_tex_scale_factor(self->dvi);
}

/**
 * @brief Get Synctex data (not available for DVI)
 *
 * DVI engines don't support Synctex synchronization.
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
 * @brief Find a file (not available for DVI)
 *
 * DVI engines don't track source files.
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
 * @brief Notify file changes (not available for DVI)
 *
 * DVI engines don't track source files.
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
 * @brief Create a new DVI engine from a file
 *
 * Opens and loads the DVI file, initializes the internal DVI parser,
 * and creates the engine instance.
 *
 * @param ctx      MuPDF context for memory allocation
 * @param dvi_path Path to the DVI file to display
 * @param loader   DVI resource loader for custom resource loading
 * @return Engine instance, or NULL on error
 */
Engine *create_dvi_engine(fz_context *ctx,
                          const char *dvi_path,
                          dvi_resloader loader)
{
  fz_buffer *buffer = fz_read_file(ctx, dvi_path);
  struct DviEngine *self = fz_malloc_struct(ctx, struct DviEngine);
  self->_class = &_class;
  self->buffer = buffer;
  self->dvi = incdvi_new(ctx, loader);
  incdvi_update(ctx, self->dvi, buffer);
  return (Engine*)self;
}
