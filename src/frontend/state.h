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

#ifndef STATE_H
#define STATE_H

#include <sys/stat.h>
#include <mupdf/fitz/buffer.h>
#include "sprotocol.h"

#define MAX_FILES 1024

enum accesslevel {
  FILE_NONE,
  FILE_READ,
  FILE_WRITE
};

typedef int LogMark;

typedef struct fileentry_s {
  const char *path;

  // Cache of filesystem state
  struct stat fs_stat;
  fz_buffer *fs_data;

  // Cached picture information
  struct pic_cache pic_cache;

  // State of the file in the text editor (or NULL if unedited)
  fz_buffer *edit_data;

  // State observed and/or produced by TeX process
  struct {
    fz_buffer *data;
    enum accesslevel level;
    LogMark snap;
  } saved;

  int seen;
  int debug_rollback_invalidation;
} FileEntry;

typedef struct filecell_s {
  LogMark snap;
  FileEntry *entry;
} FileCell;

typedef struct {
  FileCell table[MAX_FILES];
  FileCell stdout, document, synctex, log;
} TexState;

void state_init(TexState *st);

typedef struct filesystem_s FileSystem;
typedef struct log_s Log;

FileSystem *filesystem_new(fz_context *ctx);
void filesystem_free(fz_context *ctx, FileSystem *fs);
FileEntry *filesystem_lookup_or_create(fz_context *ctx, FileSystem *fs, const char *path);
FileEntry *filesystem_lookup(FileSystem *fs, const char *path);
FileEntry *filesystem_scan(FileSystem *fs, int *index);

Log *log_new(fz_context *ctx);
void log_free(fz_context *ctx, Log *log);
LogMark log_snapshot(fz_context *ctx, Log *log);
void log_rollback(fz_context *ctx, Log *log, LogMark snapshot);
void log_fileentry(fz_context *ctx, Log *log, FileEntry *entry);
void log_filecell(fz_context *ctx, Log *log, FileCell *cell);
void log_overwrite(fz_context *ctx, Log *log, fz_buffer *buf, int start, int len);

bool stat_same(struct stat *st1, struct stat *st2);

#endif /*!STATE_H*/
