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
 * @file engine.h
 * @brief Generic document engine interface and implementation
 *
 * This header defines the abstract engine interface used by TeXpresso to
 * support multiple document formats (TeX, PDF, DVI). Each engine type
 * implements a common set of operations for document lifecycle management,
 * rendering, and change detection.
 */

#ifndef GENERIC_ENGINE_H_
#define GENERIC_ENGINE_H_

#include <mupdf/fitz/display-list.h>
#include "mydvi.h"
#include "state.h"
#include "synctex.h"

/**
 * @brief Send a method call to an object using virtual dispatch
 *
 * This macro implements a simple form of virtual method dispatch based on
 * the first argument's class pointer. It's used throughout the codebase
 * to call engine methods without directly exposing the class structure.
 * @par Example:
 * @code
 *   send(step, engine, ctx, true)  // Calls engine->class->step(...)
 * @endcode
 */
#define send(method, ...)                 \
  (send__extract_first(__VA_ARGS__, NULL) \
       ->_class->method((txp_engine *)__VA_ARGS__))

#define send__extract_first(x, ...) (x)

typedef struct txp_engine_s txp_engine;

/**
 * @brief Create a new TeX engine for compiling documents
 * @param ctx            MuPDF context for memory allocation
 * @param engine_path    Path to the TeX engine executable (xetex or platex)
 * @param use_texlive    If true, use teTeX/TexLive command-line interface;
 *                       If false, use tectonic interface
 * @param inclusion_path Additional directories to search for included files
 * @param tex_name       Name of the main .tex input file
 * @param hooks          DVI rendering hooks for custom behavior
 * @return Engine instance if successful, NULL otherwise
 *
 * This creates an engine that can compile TeX source files incrementally.
 * The engine runs as a separate process and communicates with the main
 * application through a protocol-based channel.
 */
txp_engine *txp_create_tex_engine(fz_context *ctx,
                                  const char *engine_path,
                                  bool use_texlive,
                                  bool stream_mode,
                                  const char *inclusion_path,
                                  const char *tex_name,
                                  dvi_reshooks hooks);

/**
 * @brief Create a new PDF engine for displaying PDF documents
 * @param ctx      MuPDF context for memory allocation
 * @param pdf_path Path to the PDF file to display
 * @return Engine instance if successful, NULL otherwise
 *
 * This creates a read-only engine for displaying pre-compiled PDF documents.
 * It uses MuPDF's built-in PDF rendering capabilities.
 */
txp_engine *txp_create_pdf_engine(fz_context *ctx, const char *pdf_path);

/**
 * @brief Create a new DVI engine for displaying DVI documents
 * @param ctx      MuPDF context for memory allocation
 * @param dvi_path Path to the DVI file to display
 * @param hooks    DVI rendering hooks for custom behavior
 * @return Engine instance if successful, NULL otherwise
 *
 * This creates a read-only engine for displaying pre-compiled DVI documents.
 * DVI files are intermediate output format from TeX compilers.
 */
txp_engine *txp_create_dvi_engine(fz_context *ctx,
                                  const char *dvi_path,
                                  dvi_reshooks hooks);

/**
 * @brief Current status of a document engine
 */
typedef enum
{
  DOC_RUNNING,   /**< Engine is actively processing */
  DOC_TERMINATED /**< Engine has finished or failed */
} txp_engine_status;

/**
 * @brief Base engine structure
 *
 * This is the common header for all engine implementations. It contains
 * only a pointer to the virtual method table (class structure).
 * Actual engine implementations embed this structure as their first member.
 */
struct txp_engine_s
{
  struct txp_engine_class *_class;
};

/**
 * @brief Virtual method table for engine implementations
 *
 * This structure defines the interface that all engine types must implement.
 * Each method handles a specific aspect of document lifecycle management.
 */
struct txp_engine_class
{
  /**
   * @brief Destroy the engine and free all associated resources
   * @param self    Engine instance
   * @param ctx     MuPDF context
   *
   * Called when the engine is no longer needed. Must free all memory
   * allocated during engine initialization and operation.
   */
  void (*destroy)(txp_engine *self, fz_context *ctx);

  /**
   * @brief Advance the engine by processing one message from the child process
   * @param self              Engine instance
   * @param ctx               MuPDF context
   * @param restart_if_needed If true, restart the process if not running
   * @return true if a message was processed, false otherwise
   *
   * This is the main event loop function for incremental compilation.
   * It reads one query from the child process, processes it, and sends
   * back an answer. When 'restart_if_needed' is true, it will automatically
   * spawn a new process if the engine has terminated.
   */
  bool (*step)(txp_engine *self, fz_context *ctx, bool restart_if_needed);

  /**
   * @brief Begin a transaction for batch file change detection
   * @param self    Engine instance
   * @param ctx     MuPDF context
   *
   * Call this before making multiple file changes that should be treated
   * as a single atomic operation. After all changes are detected, call
   * end_changes() to commit or rollback.
   */
  void (*begin_changes)(txp_engine *self, fz_context *ctx);

  /**
   * @brief Check if any tracked files have been modified
   * @param self    Engine instance
   * @param ctx     MuPDF context
   *
   * Scans all tracked files for changes by comparing file metadata.
   * Should only be called between begin_changes() and end_changes().
   */
  void (*detect_changes)(txp_engine *self, fz_context *ctx);

  /**
   * @brief End a transaction and apply or revert changes
   * @param self    Engine instance
   * @param ctx     MuPDF context
   * @return true if changes were applied, false if reverted
   *
   * This finalizes the transaction started by begin_changes(). If any
   * changes were detected, it will rollback the engine state to before
   * the changes and restart compilation. Returns true if changes were
   * actually applied, false if compilation was rolled back instead.
   */
  bool (*end_changes)(txp_engine *self, fz_context *ctx);

  /**
   * @brief Get the total number of pages in the document
   * @param self    Engine instance
   * @return Number of pages
   *
   * For TeX engines, this represents the number of pages in the
   * current DVI output. For PDF engines, it's the number of pages
   * in the PDF document.
   */
  int (*page_count)(txp_engine *self);

  /**
   * @brief Render a page to a display list for later drawing
   * @param self    Engine instance
   * @param ctx     MuPDF context
   * @param page    Page number (0-indexed)
   * @return Display list containing the page content
   *
   * The returned display list must be freed using fz_drop_display_list()
   * when no longer needed. The display list can be rendered multiple times
   * to different devices or at different scales.
   */
  fz_display_list *(*render_page)(txp_engine *self, fz_context *ctx, int page);

  /**
   * @brief Get the current execution status of the engine
   * @param self    Engine instance
   * @return Engine status (running or terminated)
   */
  txp_engine_status (*get_status)(txp_engine *self);

  /**
   * @brief Get the scaling factor for the document
   * @param self    Engine instance
   * @return Scaling factor (typically DPI ratio)
   *
   * This factor is used to convert between device pixels and document
   * units. For TeX documents, this typically returns the TeX point
   * scaling factor. PDF documents usually return 1.0.
   */
  float (*scale_factor)(txp_engine *self);

  /**
   * @brief Get the Synctex data structure for forward/backward search
   * @param self    Engine instance
   * @param buf     If non-NULL, set to buffer containing raw Synctex data
   * @return Synctex parser instance
   *
   * Synctex enables synchronization between the source files and the
   * rendered output. The returned instance should not be freed by caller.
   */
  synctex_t *(*synctex)(txp_engine *self, fz_buffer **buf);

  /**
   * @brief Look up or create a file entry for tracking
   * @param self    Engine instance
   * @param ctx     MuPDF context
   * @param path    File path to look up
   * @return File entry for the specified path
   *
   * This function ensures a file entry exists in the filesystem
   * tracking structure. If the file is already tracked, returns the
   * existing entry; otherwise creates a new one.
   */
  fileentry_t *(*find_file)(txp_engine *self,
                            fz_context *ctx,
                            const char *path);

  /**
   * @brief Notify the engine that a file has been modified
   * @param self    Engine instance
   * @param ctx     MuPDF context
   * @param entry   File entry that was modified
   * @param offset  Byte offset where change occurred (0 for full file)
   *
   * Called by the editor when the user modifies a source file.
   * The engine uses this to determine what needs to be recompiled.
   */
  void (*notify_file_changes)(txp_engine *self,
                              fz_context *ctx,
                              fileentry_t *entry,
                              int offset);
};

/**
 * @brief Declare the standard engine class methods
 *
 * This macro declares all the virtual method implementations needed
 * for a typical engine type. It's used in engine implementation files
 * to avoid repetitive method declarations.
 *
 * Example usage:
 * @code
 * TXP_ENGINE_DEF_CLASS;
 * #define SELF struct my_engine *self = (struct my_engine*)_self
 * // Then implement each declared method
 * @endcode
 */
#define TXP_ENGINE_DEF_CLASS                                                \
  static void engine_destroy(txp_engine *_self, fz_context *ctx);           \
  static fz_display_list *engine_render_page(txp_engine *_self,             \
                                             fz_context *ctx, int page);    \
  static bool engine_step(txp_engine *_self, fz_context *ctx,               \
                          bool restart_if_needed);                          \
  static void engine_begin_changes(txp_engine *_self, fz_context *ctx);     \
  static void engine_detect_changes(txp_engine *_self, fz_context *ctx);    \
  static bool engine_end_changes(txp_engine *_self, fz_context *ctx);       \
  static int engine_page_count(txp_engine *_self);                          \
  static txp_engine_status engine_get_status(txp_engine *_self);            \
  static float engine_scale_factor(txp_engine *_self);                      \
  static synctex_t *engine_synctex(txp_engine *_self, fz_buffer **buf);     \
  static fileentry_t *engine_find_file(txp_engine *_self, fz_context *ctx,  \
                                       const char *path);                   \
  static void engine_notify_file_changes(txp_engine *self, fz_context *ctx, \
                                         fileentry_t *entry, int offset);   \
                                                                            \
  static struct txp_engine_class _class = {                                 \
      .destroy = engine_destroy,                                            \
      .step = engine_step,                                                  \
      .page_count = engine_page_count,                                      \
      .render_page = engine_render_page,                                    \
      .get_status = engine_get_status,                                      \
      .scale_factor = engine_scale_factor,                                  \
      .synctex = engine_synctex,                                            \
      .find_file = engine_find_file,                                        \
      .begin_changes = engine_begin_changes,                                \
      .detect_changes = engine_detect_changes,                              \
      .end_changes = engine_end_changes,                                    \
      .notify_file_changes = engine_notify_file_changes,                    \
  }

#endif  // GENERIC_ENGINE_H_
