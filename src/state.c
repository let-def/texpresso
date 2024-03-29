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

#include <stdlib.h>
#include "state.h"
#include "string.h"
#include "mupdf_compat.h"
#include "dvi/fz_util.h"

/* Rollback log */

#undef LOG
#define LOG 0

enum log_action {
  LOG_ENTRY = 0x42,
  LOG_CELL,
  LOG_OVERWRITE
};

struct log_s
{
  mark_t snap;
  fz_buffer *data;
};

log_t *log_new(fz_context *ctx)
{
  fz_ptr(log_t, log);
  fz_ptr(fz_buffer, data);
  fz_try(ctx)
  {
    log = fz_malloc_struct(ctx, log_t);
    data = fz_new_buffer(ctx, 512);
    log->data = data;
    log->snap = 1;
    fz_append_byte(ctx, log->data, 0);
  }
  fz_catch(ctx)
  {
    if (data)
      fz_drop_buffer(ctx, data);
    if (log)
      fz_free(ctx, log);
    fz_rethrow(ctx);
  }
  return log;
}

void log_free(fz_context *ctx, log_t *log)
{
  fz_drop_buffer(ctx, log->data);
  fz_free(ctx, log);
}

#define PUSH_VALUE(ctx, buf, val) \
  fz_append_data(ctx, buf, &(val), sizeof(val))

static void pop_value(fz_buffer *buf, void *val, size_t len)
{
  if (buf->len < len) abort();
  buf->len -= len;
  memcpy(val, buf->data + buf->len, len);
}

#define POP_VALUE(buf, val) \
  pop_value(buf, &(val), sizeof(val))

static void push_action(fz_context *ctx, fz_buffer *buf, enum log_action action)
{
  uint8_t b = action;
  PUSH_VALUE(ctx, buf, b);
}

void log_fileentry(fz_context *ctx, log_t *log, fileentry_t *entry)
{
  if (entry->saved.snap != log->snap)
  {
    if (LOG) fprintf(stderr, "push LOG_ENTRY %s\n", entry->path);
    if (entry->saved.data)
    {
      fz_keep_buffer(ctx, entry->saved.data);
      PUSH_VALUE(ctx, log->data, entry->saved.data->len);
    }
    PUSH_VALUE(ctx, log->data, entry->saved);
    PUSH_VALUE(ctx, log->data, entry);
    push_action(ctx, log->data, LOG_ENTRY);
    entry->saved.snap = log->snap;
  }
}

void log_filecell(fz_context *ctx, log_t *log, filecell_t *cell)
{
  if (cell->snap != log->snap)
  {
    if (LOG) fprintf(stderr, "push LOG_CELL\n");
    PUSH_VALUE(ctx, log->data, *cell);
    PUSH_VALUE(ctx, log->data, cell);
    push_action(ctx, log->data, LOG_CELL);
    cell->snap = log->snap;
  }
}

struct overwrite_data {
  fz_buffer *buf;
  int start, len;
};

void log_overwrite(fz_context *ctx, log_t *log, fz_buffer *buf, int start, int len)
{
  if (LOG) fprintf(stderr, "push LOG_OVERWRITE\n");
  fz_keep_buffer(ctx, buf);
  fz_append_data(ctx, log->data, buf->data + start, len);
  struct overwrite_data data = {
    .buf = buf,
    .start = start,
    .len = len,
  };
  PUSH_VALUE(ctx, log->data, data);
  push_action(ctx, log->data, LOG_OVERWRITE);
}

static enum log_action pop_action(fz_buffer *buf)
{
  uint8_t b;
  POP_VALUE(buf, b);
  return b;
}

static void log_pop(fz_context *ctx, log_t *log)
{
  switch (pop_action(log->data))
  {
    case LOG_ENTRY:
    {
      fileentry_t *entry;
      POP_VALUE(log->data, entry);
      if (LOG) fprintf(stderr, "pop LOG_ENTRY %s\n", entry->path);
      if (entry->saved.data)
        fz_drop_buffer(ctx, entry->saved.data);
      POP_VALUE(log->data, entry->saved);
      if (entry->saved.data)
      {
        POP_VALUE(log->data, entry->saved.data->len);
      }
      break;
    }
    case LOG_CELL:
    {
      if (LOG) fprintf(stderr, "pop LOG_CELL\n");
      filecell_t *cell;
      POP_VALUE(log->data, cell);
      POP_VALUE(log->data, *cell);
      break;
    }
    case LOG_OVERWRITE:
    {
      if (LOG) fprintf(stderr, "pop LOG_OVERWRITE\n");
      struct overwrite_data data;
      POP_VALUE(log->data, data);
      pop_value(log->data, data.buf->data + data.start, data.len);
      fz_drop_buffer(ctx, data.buf);
      break;
    }
    default:
      abort();
  }
}

mark_t log_snapshot(fz_context *ctx, log_t *log)
{
  return (log->snap = log->data->len);
}

void log_rollback(fz_context *ctx, log_t *log, mark_t mark)
{
  if (mark > log->snap) abort();

  while (log->data->len > mark)
    log_pop(ctx, log);

  if (mark != log->data->len)
  {
    fprintf(stderr, "[fatal] rollback: mark=%d len =%d\n", mark, (int)log->data->len);
    abort();
  }

  log->snap = mark;
}

/* State */

void state_init(state_t *st)
{
  memset(st, 0, sizeof(state_t));
}

static bool
same_time(struct timespec a, struct timespec b)
{
  return (a.tv_sec == b.tv_sec) && (a.tv_nsec == b.tv_nsec);
}

#ifndef __APPLE__
# define st_time(a) st_##a##tim
#else
# define st_time(a) st_##a##timespec
#endif

bool stat_same(struct stat *st1, struct stat *st2)
{
  return st1->st_dev     == st2->st_dev &&
         st1->st_ino     == st2->st_ino &&
         st1->st_mode    == st2->st_mode &&
         st1->st_nlink   == st2->st_nlink &&
         st1->st_uid     == st2->st_uid &&
         st1->st_gid     == st2->st_gid &&
         st1->st_rdev    == st2->st_rdev &&
         st1->st_size    == st2->st_size &&
         st1->st_blksize == st2->st_blksize &&
         st1->st_blocks  == st2->st_blocks &&
         same_time(st1->st_time(a), st2->st_time(a)) &&
         same_time(st1->st_time(m), st2->st_time(m)) &&
         same_time(st1->st_time(c), st2->st_time(c));
}
