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
 * @brief Generic document engine interface
 *
 * This header defines the abstract engine interface used by TeXpresso to
 * support multiple document formats (TeX, PDF, DVI). Each engine type
 * implements a common set of operations for document lifecycle management,
 * rendering, and change detection.
 *
 * Engine Architecture:
 * - Abstract class-based design using virtual method table
 * - Multiple concrete implementations for different document formats
 * - Protocol-based communication for TeX engine (child process)
 *
 * @see engine_tex.c TeX incremental compilation engine
 * @see engine_pdf.c PDF display engine
 * @see engine_dvi.c DVI display engine
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
 *
 * @par Example:
 * @code
 *   send(step, engine, ctx, true)  // Calls engine->_class->step(...)
 * @endcode
 *
 * @param method Name of the method to call
 * @param ...    Engine instance followed by additional arguments
 */
#define send(method, ...)                 \
  (send__extract_first(__VA_ARGS__, NULL) \
        ->_class->method((Engine *)__VA_ARGS__))

#define send__extract_first(x, ...) (x)

typedef struct Engine Engine;

/**
 * @brief Create a new TeX engine for compiling documents
 *
 * Creates an incremental compilation engine that runs a TeX compiler
 * (xetex or platex) as a child process with bidirectional communication.
 * Supports checkpointing and rollback for efficient incremental recompilation.
 *
 * @param ctx            MuPDF context for memory allocation
 * @param engine_path    Path to the TeX engine executable (xetex or platex)
 * @param use_texlive    If true, use teTeX/TexLive command-line interface;
 *                       If false, use tectonic interface
 * @param stream_mode    If true, enable stream mode for VFS data handling
 * @param inclusion_path Additional directories to search for included files
 * @param tex_name       Name of the main .tex input file
 * @param hooks          DVI resource hooks for custom resource loading
 * @return Engine instance if successful, NULL otherwise
 *
 * @see engine_tex.c Implementation details
 * @see create_dvi_engine For displaying pre-compiled DVI files
 * @see create_pdf_engine For displaying pre-compiled PDF files
 */
Engine *create_tex_engine(fz_context *ctx,
                          const char *engine_path,
                          bool use_texlive,
                          bool stream_mode,
                          const char *inclusion_path,
                          const char *tex_name,
                          dvi_reshooks hooks);

/**
 * @brief Create a new PDF engine for displaying PDF documents
 *
 * Creates a read-only engine for displaying pre-compiled PDF documents.
 * Uses MuPDF's built-in PDF rendering capabilities with no incremental
 * compilation support.
 *
 * @param ctx      MuPDF context for memory allocation
 * @param pdf_path Path to the PDF file to display
 * @return Engine instance if successful, NULL otherwise
 *
 * @see create_tex_engine For incremental TeX compilation
 * @see create_dvi_engine For displaying pre-compiled DVI files
 */
Engine *create_pdf_engine(fz_context *ctx, const char *pdf_path);

/**
 * @brief Create a new DVI engine for displaying DVI documents
 *
 * Creates a read-only engine for displaying pre-compiled DVI documents.
 * DVI files are intermediate output format from TeX compilers.
 *
 * @param ctx      MuPDF context for memory allocation
 * @param dvi_path Path to the DVI file to display
 * @param hooks    DVI resource hooks for custom resource loading
 * @return Engine instance if successful, NULL otherwise
 *
 * @see create_tex_engine For incremental TeX compilation
 * @see create_pdf_engine For displaying pre-compiled PDF files
 */
Engine *create_dvi_engine(fz_context *ctx,
                          const char *dvi_path,
                          dvi_reshooks hooks);

/**
 * @brief Current status of a document engine
 *
 * Represents the execution state of the engine:
 * - DOC_RUNNING: Engine is actively processing (TeX engine) or available
 * - DOC_TERMINATED: Engine has finished or encountered an error
 */
typedef enum
{
  DOC_RUNNING,   /**< Engine is actively processing */
  DOC_TERMINATED /**< Engine has finished or failed */
} EngineStatus;

/**
 * @brief Base engine structure
 *
 * This is the common header for all engine implementations. It contains
 * only a pointer to the virtual method table (class structure).
 * Actual engine implementations embed this structure as their first member.
 */
struct Engine
{
  struct EngineClass *_class;
};

/**
 * @brief Virtual method table for engine implementations
 *
 * This structure defines the interface that all engine types must implement.
 * Each method handles a specific aspect of document lifecycle management.
 *
 * The virtual dispatch is implemented via the send() macro which uses
 * the first argument's _class pointer to look up the function pointer.
 *
 * @see Engine The base engine structure
 * @see send Macro for virtual method dispatch
 */
struct EngineClass
{
  /**
   * @brief Destroy the engine and free all associated resources
   *
   * Called when the engine is no longer needed. Must free all memory
   * allocated during engine initialization and operation, including:
   * - Internal data structures
   * - MuPDF resources (documents, display lists, etc.)
   * - Child process resources (TeX engine only)
   *
   * @param self Engine instance
   * @param ctx  MuPDF context for resource deallocation
   */
  void (*destroy)(Engine *self, fz_context *ctx);

  /**
   * @brief Advance the engine by processing one message from the child process
   *
   * This is the main event loop function for incremental compilation.
   * It reads one query from the child process, processes it, and sends
   * back an answer. When 'restart_if_needed' is true, it will automatically
   * spawn a new process if the engine has terminated.
   *
   * @note For PDF and DVI engines, this always returns false as they
   *       don't have child processes.
   *
   * @param self              Engine instance
   * @param ctx               MuPDF context
   * @param restart_if_needed If true, restart the process if not running
   * @return true if a message was processed, false otherwise
   */
  bool (*step)(Engine *self, fz_context *ctx, bool restart_if_needed);

  /**
   * @brief Begin a transaction for batch file change detection
   *
   * Call this before making multiple file changes that should be treated
   * as a single atomic operation. After all changes are detected, call
   * end_changes() to commit or rollback.
   *
   * During a transaction, the engine records the current state so that
   * it can rollback if changes are detected. Multiple changes can be
   * detected and applied in a single transaction.
   *
   * @note Only the TeX engine implements full transaction support.
   *       PDF and DVI engines provide empty stubs.
   *
   * @param self Engine instance
   * @param ctx  MuPDF context
   *
   * @see detect_changes Scan for file modifications
   * @see end_changes  Commit or rollback changes
   */
  void (*begin_changes)(Engine *self, fz_context *ctx);

  /**
   * @brief Check if any tracked files have been modified
   *
   * Scans all tracked files for changes by comparing file metadata.
   * Should only be called between begin_changes() and end_changes().
   *
   * The engine compares current file timestamps and sizes against
   * previously recorded values. Any changes trigger a rollback and
   * recompilation.
   *
   * @note Only the TeX engine implements file scanning. PDF and DVI
   *       engines provide empty stubs as they don't track source files.
   *
   * @param self Engine instance
   * @param ctx  MuPDF context
   *
   * @see begin_changes Start a change transaction
   * @see end_changes   Commit or rollback changes
   */
  void (*detect_changes)(Engine *self, fz_context *ctx);

  /**
   * @brief End a transaction and apply or revert changes
   *
   * This finalizes the transaction started by begin_changes(). If any
   * changes were detected, it will rollback the engine state to before
   * the changes and restart compilation. Returns true if changes were
   * actually applied, false if compilation was rolled back instead.
   *
   * @note For TeX engine: returns true when changes are applied (requiring
   *       recompilation). For PDF/DVI engines: returns false as there's
   *       no recompilation needed.
   *
   * @param self Engine instance
   * @param ctx  MuPDF context
   * @return true if changes were applied (TeX engine recompiling),
   *         false if no changes detected or rollback occurred
   *
   * @see begin_changes Start a change transaction
   * @see detect_changes Scan for file modifications
   */
  bool (*end_changes)(Engine *self, fz_context *ctx);

  /**
   * @brief Get the total number of pages in the document
   *
   * For TeX engines, this represents the number of pages in the
   * current DVI output. For PDF engines, it's the number of pages
   * in the PDF document. DVI engines return the page count from
   * their internal DVI parser.
   *
   * @param self Engine instance
   * @return Number of pages (0 if document not loaded or empty)
   *
   * @see render_page Render a specific page
   */
  int (*page_count)(Engine *self);

  /**
   * @brief Render a page to a display list for later drawing
   *
   * The returned display list contains the page's visual content
   * and can be rendered multiple times to different devices or
   * at different scales. The caller owns the display list and
   * must free it using fz_drop_display_list() when no longer needed.
   *
   * @param self Engine instance
   * @param ctx  MuPDF context for rendering operations
   * @param page Page number (0-indexed)
   * @return Display list containing the page content, or NULL on error
   *
   * @note Display lists should be freed with fz_drop_display_list()
   *       after use to avoid memory leaks.
   *
   * @see page_count Get total number of pages
   * @see fz_drop_display_list Free the display list
   */
  fz_display_list *(*render_page)(Engine *self, fz_context *ctx, int page);

  /**
   * @brief Get the current execution status of the engine
   *
   * For TeX engines, returns DOC_RUNNING while the child process
   * is alive and processing, DOC_TERMINATED when it has finished.
   * PDF and DVI engines always return DOC_TERMINATED as they don't
   * run separate processes.
   *
   * @param self Engine instance
   * @return Current engine status (running or terminated)
   */
  EngineStatus (*get_status)(Engine *self);

  /**
   * @brief Get the scaling factor for the document
   *
   * This factor is used to convert between device pixels and document
   * units. For TeX documents, this typically returns the TeX point
   * scaling factor. PDF documents usually return 1.0, and DVI engines
   * return the scaling factor from their configuration.
   *
   * @param self Engine instance
   * @return Scaling factor (pixels per document unit)
   *
   * @note The exact units depend on the engine type. Use with
   *       page dimensions obtained from render_page() to compute
   *       proper rendering sizes.
   */
  float (*scale_factor)(Engine *self);

  /**
   * @brief Get the Synctex data structure for forward/backward search
   *
   * Synctex enables synchronization between the source files and the
   * rendered output. The returned instance should not be freed by caller.
   *
   * @param self Engine instance
   * @param buf  If non-NULL, set to buffer containing raw Synctex data
   * @return Synctex parser instance, or NULL if not available
   *
   * @note Only TeX engines generate Synctex data. PDF and DVI engines
   *       return NULL as they don't support source synchronization.
   */
  TexSynctex *(*synctex)(Engine *self, fz_buffer **buf);

  /**
   * @brief Look up or create a file entry for tracking
   *
   * This function ensures a file entry exists in the filesystem
   * tracking structure. If the file is already tracked, returns the
   * existing entry; otherwise creates a new one.
   *
   * @param self Engine instance
   * @param ctx  MuPDF context for memory allocation
   * @param path File path to look up
   * @return File entry for the specified path
   *
   * @note Only the TeX engine tracks files. PDF and DVI engines
   *       return NULL as they don't monitor source files.
   */
  FileEntry *(*find_file)(Engine *self,
                            fz_context *ctx,
                            const char *path);

  /**
   * @brief Notify the engine that a file has been modified
   *
   * Called by the editor when the user modifies a source file.
   * The engine uses this to determine what needs to be recompiled.
   * The offset parameter indicates where the change occurred (0
   * means the entire file may have changed).
   *
   * @param self  Engine instance
   * @param ctx   MuPDF context
   * @param entry File entry that was modified
   * @param offset Byte offset where change occurred (0 for full file)
   *
   * @note Only the TeX engine implements this. PDF and DVI engines
   *       have empty implementations.
   */
void (*notify_file_changes)(Engine *self,
                            fz_context *ctx,
                            FileEntry *entry,
                              int offset);
};

/**
 * @brief Declare the standard engine class methods
 *
 * This macro declares all the virtual method implementations needed
 * for a typical engine type. It's used in engine implementation files
 * to avoid repetitive method declarations.
 *
 * @par Example:
 * @code
 * TXP_ENGINE_DEF_CLASS;
 * #define SELF struct my_engine *self = (struct my_engine*)_self
 *
 * // Then implement each declared method
 * static void engine_destroy(Engine *_self, fz_context *ctx) {
 *   SELF;
 *   // implementation...
 * }
 * // ... other methods ...
 *
 * // Finally, initialize the class structure
 * static struct EngineClass _class = {
 *   .destroy = engine_destroy,
 *   // ... initialize all method pointers
 * };
 * @endcode
 *
 * @note This macro only declares the methods and static _class instance.
 *       Each method must still be implemented separately.
 */
#define TXP_ENGINE_DEF_CLASS                                                 \
  static void engine_destroy(Engine *_self, fz_context *ctx);                \
  static fz_display_list *engine_render_page(Engine *_self, fz_context *ctx, \
                                             int page);                      \
  static bool engine_step(Engine *_self, fz_context *ctx,                    \
                          bool restart_if_needed);                           \
  static void engine_begin_changes(Engine *_self, fz_context *ctx);          \
  static void engine_detect_changes(Engine *_self, fz_context *ctx);         \
  static bool engine_end_changes(Engine *_self, fz_context *ctx);            \
  static int engine_page_count(Engine *_self);                               \
  static EngineStatus engine_get_status(Engine *_self);                      \
  static float engine_scale_factor(Engine *_self);                           \
  static TexSynctex *engine_synctex(Engine *_self, fz_buffer **buf);         \
  static FileEntry *engine_find_file(Engine *_self, fz_context *ctx,         \
                                     const char *path);                      \
  static void engine_notify_file_changes(Engine *self, fz_context *ctx,      \
                                         FileEntry *entry, int offset);      \
                                                                             \
  static struct EngineClass _class = {                                       \
      .destroy = engine_destroy,                                             \
      .step = engine_step,                                                   \
      .page_count = engine_page_count,                                       \
      .render_page = engine_render_page,                                     \
      .get_status = engine_get_status,                                       \
      .scale_factor = engine_scale_factor,                                   \
      .synctex = engine_synctex,                                             \
      .find_file = engine_find_file,                                         \
      .begin_changes = engine_begin_changes,                                 \
      .detect_changes = engine_detect_changes,                               \
      .end_changes = engine_end_changes,                                     \
      .notify_file_changes = engine_notify_file_changes,                     \
  }

#endif  // GENERIC_ENGINE_H_
