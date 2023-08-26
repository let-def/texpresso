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

#include <mupdf/fitz/buffer.h>
#include <string.h>
#include "vstack.h"

static inline void store_u08(uint8_t *dest, uint8_t v)
{
  dest[0] = v;
}

// static inline void store_u16(uint8_t *dest, uint16_t v)
// {
//   dest[0] = v & 0xFF;
//   dest[1] = (v >> 8) & 0xFF;
// }

static inline void store_u24(uint8_t *dest, uint32_t v)
{
  dest[0] = v & 0xFF;
  dest[1] = (v >> 8) & 0xFF;
  dest[2] = (v >> 16) & 0xFF;
}

static inline void store_u32(uint8_t *dest, uint32_t v)
{
  dest[0] = v & 0xFF;
  dest[1] = (v >> 8) & 0xFF;
  dest[2] = (v >> 16) & 0xFF;
  dest[3] = (v >> 24) & 0xFF;
}

static inline uint8_t load_u08(const uint8_t *p)
{
  return p[0];
}

// static inline uint16_t load_u16(const uint8_t *p)
// {
//   return p[0] | (p[1] << 8);
// }

static inline uint32_t load_u24(const uint8_t *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16);
}

static inline uint32_t load_u32(const uint8_t *p)
{
  return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

enum
{
  TAG_NULL,
  TAG_TRUE,
  TAG_FALSE,
  TAG_NUMBER,
  TAG_STRING,
  TAG_HEXSTRING,
  TAG_NAME,
  TAG_ARRAY,
  TAG_DICT,
  TAG_REF,
};

enum val_string_kind
{
  CTX_NONE,
  CTX_STRING,
  CTX_HEXSTRING,
  CTX_NAME,
};

enum val_struct_kind
{
  CTX_TOP,
  CTX_ARRAY,
  CTX_DICT,
};

struct vstack
{
  enum val_string_kind string_kind;
  uint32_t string_length;

  enum val_struct_kind struct_kind;
  uint32_t struct_length;
  uint32_t previous;

  uint8_t *data;
  size_t len, cap;
};

void vstack_reset(fz_context *ctx, vstack *t)
{
  t->string_kind = CTX_NONE;
  t->string_length = 0;
  t->struct_kind = CTX_TOP;
  t->struct_length = 0;
  t->previous = 0;
  t->len = 0;
}

vstack *vstack_new(fz_context *ctx)
{
  vstack *t = fz_malloc_struct(ctx, vstack);
  t->data = NULL;
  t->cap = 0;
  vstack_reset(ctx, t);
  return t;
}

void vstack_free(fz_context *ctx, vstack *t)
{
  if (t->data)
    fz_free(ctx, t->data);
  fz_free(ctx, t);
}

static uint8_t *vstack_alloc(fz_context *ctx, vstack *t, size_t len)
{
  size_t offset = t->len;
  t->len += len;
  if (t->len > t->cap)
  {
    if (t->cap == 0)
      t->cap = 128;
    while (t->cap < t->len)
      t->cap = t->cap * 3 / 2;
    t->data = fz_realloc(ctx, t->data, t->cap);
  }
  return &t->data[offset];
}

#define ASSERT(cond) \
  do { if (!(cond)) fz_throw(ctx, 0, "%s: invalid context", __func__); } while (0)

void vstack_push_null(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE);
  t->struct_length += 1;
  vstack_alloc(ctx, t, 1)[0] = TAG_NULL;
}

void vstack_push_number(fz_context *ctx, vstack *t, float value)
{
  ASSERT(t->string_kind == CTX_NONE);
  t->struct_length += 1;
  uint8_t *p = vstack_alloc(ctx, t, 5);
  p[0] = TAG_NUMBER;
  memcpy(&p[1], &value, 4);
}

void vstack_push_bool(fz_context* ctx, vstack *t, bool value)
{
  ASSERT(t->string_kind == CTX_NONE);
  t->struct_length += 1;
  vstack_alloc(ctx, t, 1)[0] = value ? TAG_TRUE : TAG_FALSE;
}

void vstack_push_ref(fz_context *ctx, vstack *t, int obj, int gen)
{
  ASSERT(t->string_kind == CTX_NONE);
  t->struct_length += 1;
  uint8_t *data = vstack_alloc(ctx, t, 8);
  data[0] = TAG_REF;
  store_u24(&data[1], gen);
  store_u32(&data[4], obj);
}

static void vstack_open_struct(fz_context *ctx, vstack *t, enum val_struct_kind kind)
{
  ASSERT(t->string_kind == CTX_NONE);
  uint8_t *data = vstack_alloc(ctx, t, 8);
  store_u08 (data + 0, t->struct_kind);
  store_u24(data + 1, t->struct_length);
  store_u32(data + 4, t->previous);
  t->struct_kind = kind;
  t->struct_length = 0;
  t->previous = t->len;
}

void vstack_begin_array(fz_context *ctx, vstack *t)
{
  vstack_open_struct(ctx, t, CTX_ARRAY);
}

void vstack_begin_dict(fz_context *ctx, vstack *t)
{
  vstack_open_struct(ctx, t, CTX_DICT);
}

static void vstack_close_struct(fz_context *ctx, vstack *t, uint8_t tag, val *out)
{
  uint32_t base = t->len, offset = t->previous, length = t->struct_length;
  uint8_t *index = vstack_alloc(ctx, t, length * 4);
  for (int i = 0; i < length; ++i)
  {
    store_u32(index, offset);
    switch (t->data[offset])
    {
      case TAG_NULL: case TAG_TRUE: case TAG_FALSE:
        offset += 1;
        break;

      case TAG_NUMBER:
        offset += 5;
        break;

      case TAG_REF:
        offset += 8;
        break;

      case TAG_STRING: case TAG_HEXSTRING: case TAG_NAME:
        offset += 1 + 3 + load_u24(&t->data[offset + 1]) + 1;
        break;

      case TAG_ARRAY: case TAG_DICT:
        offset = load_u32(&t->data[offset + 4]) + load_u24(&t->data[offset + 1]) * 4;
        break;

      default:
        fprintf(stderr, "unknown tag: %x, aborting\n", t->data[offset]);
        abort();
    }
    index += 4;
  }

  if (t->previous != 0)
  {
    if (out) abort();
    uint8_t *data = &t->data[t->previous - 8];
    t->struct_kind = load_u08(data + 0);
    t->struct_length = load_u24(data + 1) + 1;
    t->previous = load_u32(data + 4);
    store_u08(data + 0, tag);
    store_u24(data + 1, length);
    store_u32(data + 4, base);
  }
  else
  {
    if (t->struct_kind != CTX_TOP || !out || tag != 0)
      abort();
    out->kind = VAL_ARRAY;
    out->length = length;
    out->o = base;
  }
}

void vstack_end_array(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE && t->struct_kind == CTX_ARRAY);
  vstack_close_struct(ctx, t, TAG_ARRAY, NULL);
}

void vstack_end_dict(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE && t->struct_kind == CTX_DICT);
  if ((t->struct_length & 1) == 1)
    fz_throw(ctx, 0, "vstack: unterminated dict");
  vstack_close_struct(ctx, t, TAG_DICT, NULL);
}

static void vstack_open_string(fz_context *ctx, vstack *t, enum val_string_kind kind, uint8_t tag)
{
  if (t->string_length != 0) abort();
  t->string_kind = kind;
  vstack_alloc(ctx, t, 4)[0] = tag;
}

static void vstack_close_string(fz_context *ctx, vstack *t)
{
  uint32_t offset = t->len - t->string_length - 4;
  store_u24(&t->data[offset + 1], t->string_length);
  vstack_alloc(ctx, t, 1)[0] = 0;
  t->string_kind = CTX_NONE;
  t->string_length = 0;
  t->struct_length += 1;
}

void vstack_begin_string(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE);
  vstack_open_string(ctx, t, CTX_STRING, TAG_STRING);
}

void vstack_begin_hexstring(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE);
  vstack_open_string(ctx, t, CTX_HEXSTRING, TAG_HEXSTRING);
}

void vstack_begin_name(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE);
  vstack_open_string(ctx, t, CTX_NAME, TAG_NAME);
}

void vstack_end_string(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_STRING);
  vstack_close_string(ctx, t);
}

void vstack_end_hexstring(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_HEXSTRING);
  vstack_close_string(ctx, t);
}

void vstack_end_name(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NAME);
  vstack_close_string(ctx, t);
}

void vstack_push_char(fz_context *ctx, vstack *t, int c)
{
  ASSERT(t->string_kind != CTX_NONE);
  vstack_alloc(ctx, t, 1)[0] = c;
  t->string_length += 1;
}

void vstack_push_chars(fz_context *ctx, vstack *t, const void *data, size_t len)
{
  ASSERT(t->string_kind != CTX_NONE);
  if (len == 0)
    return;
  memcpy(vstack_alloc(ctx, t, len), data, len);
  t->string_length += len;
}

static val decode(const uint8_t* data, uint32_t *offset)
{
  val result;
  size_t len;
  switch (data[*offset])
  {
    case TAG_NULL:
      *offset += 1;
      return (val){.kind = VAL_NULL};

    case TAG_TRUE:
      *offset += 1;
      return (val){.kind = VAL_BOOL, .b = 1};

    case TAG_FALSE:
      *offset += 1;
      return (val){.kind = VAL_BOOL, .b = 0};

    case TAG_NUMBER:
      result.kind = VAL_NUMBER;
      memcpy(&result.f, &data[*offset + 1], 4);
      *offset += 5;
      return result;

    case TAG_REF:
      result.kind = VAL_REF;
      result.length = load_u24(&data[*offset + 1]);
      result.o = load_u32(&data[*offset + 4]);
      *offset += 8;
      return result;

    case TAG_STRING:
    case TAG_HEXSTRING:
    case TAG_NAME:
      len = load_u24(&data[*offset + 1]);
      result.kind = VAL_STRING + data[*offset] - TAG_STRING;
      result.length = len;
      *offset += 4;
      result.o = *offset;
      *offset += len + 1;
      return result;
      break;

    case TAG_ARRAY:
    case TAG_DICT:
      len = load_u24(&data[*offset + 1]);
      result.kind = VAL_ARRAY + data[*offset] - TAG_ARRAY;
      result.length = len;
      result.o = load_u32(&data[*offset + 4]);
      *offset = result.o + len * 4;
      return result;
      break;

    default:
      abort();
  }
}

val vstack_get_values(fz_context *ctx, vstack *t)
{
  ASSERT(t->string_kind == CTX_NONE && t->struct_kind == CTX_TOP);
  val result;
  vstack_close_struct(ctx, t, 0, &result);
  vstack_reset(ctx, t);
  return result;
}

void vstack_get_arguments(fz_context *ctx, vstack *t, val *values, int count)
{
  ASSERT(t->string_kind == CTX_NONE && t->struct_kind == CTX_TOP);
  if (t->struct_length != count)
    fz_throw(ctx, 0,
             "vstack_get_arguments: incorrect arity, expected %d, got %d",
             count, t->struct_length);
  val array = vstack_get_values(ctx, t);
  for (int i = 0; i < count; ++i)
    values[i] = val_array_get(ctx, t, array, i);
}

void vstack_get_floats(fz_context *ctx, vstack *t, float *values, int count)
{
  ASSERT(t->string_kind == CTX_NONE && t->struct_kind == CTX_TOP);
  if (t->struct_length != count)
    fz_throw(ctx, 0,
             "vstack_get_arguments: incorrect arity, expected %d, got %d",
             count, t->struct_length);
  val array = vstack_get_values(ctx, t);
  for (int i = 0; i < count; ++i)
    values[i] = val_number(ctx, val_array_get(ctx, t, array, i));
}

val val_array_get(fz_context *ctx, vstack *t, val array, int index)
{
  ASSERT(index >= 0 && index < array.length && t->len == 0);
  uint32_t offset = load_u32(&t->data[array.o + index * 4]);
  return decode(t->data, &offset);
}

val val_dict_get_key(fz_context *ctx, vstack *t, val dict, int index)
{
  ASSERT(index >= 0 && index < dict.length / 2 && t->len == 0);
  uint32_t offset = load_u32(&t->data[dict.o + index * 8]);
  return decode(t->data, &offset);
}

val val_dict_get_value(fz_context *ctx, vstack *t, val dict, int index)
{
  ASSERT(index >= 0 && index + 1 < dict.length / 2 && t->len == 0);
  uint32_t offset = load_u32(&t->data[dict.o + index * 8 + 4]);
  return decode(t->data, &offset);
}

const char *val_string(fz_context *ctx, vstack *t, val v)
{
  ASSERT(
    (v.kind == VAL_STRING || v.kind == VAL_HEXSTRING || v.kind == VAL_NAME)
    && t->len == 0
  );
  return (const char *)&t->data[v.o];
}

const char *val_as_string(fz_context *ctx, vstack *t, val v)
{
  if (!val_is_string(v))
    return NULL;
  return val_string(ctx, t, v);
}

const char *val_as_name(fz_context *ctx, vstack *t, val v)
{
  if (!val_is_name(v))
    return NULL;
  return val_string(ctx, t, v);
}

bool vstack_at_top_level(vstack *t)
{
  return (t->struct_kind == CTX_TOP);
}

bool vstack_in_string(vstack *t)
{
  return t->string_kind != CTX_NONE;
}

bool vstack_in_dict(vstack *t)
{
  return t->struct_kind == CTX_DICT;
}

bool vstack_in_array(vstack *t)
{
  return t->struct_kind == CTX_ARRAY;
}
