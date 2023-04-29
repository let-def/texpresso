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

#ifndef VSTACK_H_
#define VSTACK_H_

#include <mupdf/fitz/context.h>
#include <stdbool.h>

typedef struct vstack vstack;

vstack *vstack_new(fz_context *ctx);
void vstack_free(fz_context *ctx, vstack *t);
void vstack_reset(fz_context *ctx, vstack *t);

void vstack_push_null(fz_context *ctx, vstack *t);
void vstack_push_number(fz_context *ctx, vstack *t, float value);
void vstack_push_bool(fz_context *ctx, vstack *t, bool value);
void vstack_push_ref(fz_context *ctx, vstack *t, int obj, int gen);

void vstack_begin_array(fz_context *ctx, vstack *t);
void vstack_end_array(fz_context *ctx, vstack *t);

void vstack_begin_dict(fz_context *ctx, vstack *t);
void vstack_end_dict(fz_context *ctx, vstack *t);

void vstack_begin_string(fz_context *ctx, vstack *t);
void vstack_end_string(fz_context *ctx, vstack *t);

void vstack_begin_hexstring(fz_context *ctx, vstack *t);
void vstack_end_hexstring(fz_context *ctx, vstack *t);

void vstack_begin_name(fz_context *ctx, vstack *t);
void vstack_end_name(fz_context *ctx, vstack *t);

void vstack_push_char(fz_context *ctx, vstack *t, int c);
void vstack_push_chars(fz_context *ctx, vstack *t, const void *data, size_t len);

enum val_kind
{
  VAL_NUMBER = 2,
  VAL_BOOL,
  VAL_NULL,
  VAL_STRING,
  VAL_HEXSTRING,
  VAL_NAME,
  VAL_ARRAY,
  VAL_DICT,
  VAL_REF,
};

typedef struct
{
  enum val_kind kind : 4;
  uint32_t length : 28;
  union
  {
    float f;
    bool b;
    uint32_t o;
  };
} val;

val vstack_get_values(fz_context *ctx, vstack *t);
void vstack_get_arguments(fz_context *ctx, vstack *t, val *values, int count);
void vstack_get_floats(fz_context *ctx, vstack *t, float *values, int count);

#define inlined static inline __attribute__((unused))

inlined bool val_is_null(val v)   { return (v.kind == VAL_NULL); }
inlined bool val_is_number(val v) { return (v.kind == VAL_NUMBER); }
inlined bool val_is_bool(val v)   { return (v.kind == VAL_BOOL); }
inlined bool val_is_string(val v) { return (v.kind == VAL_STRING || v.kind == VAL_HEXSTRING); }
inlined bool val_is_name(val v)   { return (v.kind == VAL_NAME); }
inlined bool val_is_array(val v)  { return (v.kind == VAL_ARRAY); }
inlined bool val_is_dict(val v)   { return (v.kind == VAL_DICT); }
inlined bool val_is_ref(val v)    { return (v.kind == VAL_REF); }

#define VAL_CHECK(v, kind) \
  if (!val_is_##kind(v)) \
    fz_throw(ctx, 0, "%s: value is not a %s", __func__, #kind)

inlined float val_number(fz_context *ctx, val v)
{ VAL_CHECK(v, number); return v.f; }

inlined bool val_bool(fz_context *ctx, val v)
{ VAL_CHECK(v, bool); return v.b; }

inlined uint32_t val_string_length(fz_context *ctx, vstack *t, val v)
{ VAL_CHECK(v, string); return v.length; }

inlined uint32_t val_array_length(fz_context *ctx, vstack *t, val v)
{ VAL_CHECK(v, array); return v.length; }

inlined uint32_t val_dict_length(fz_context *ctx, vstack *t, val v)
{ VAL_CHECK(v, dict); return v.length; }

inlined uint32_t val_ref_obj(fz_context *ctx, vstack *t, val v)
{ VAL_CHECK(v, dict); return v.o; }

inlined uint32_t val_ref_gen(fz_context *ctx, vstack *t, val v)
{ VAL_CHECK(v, dict); return v.length; }

#undef inlined
#undef VAL_CHECK

bool vstack_at_top_level(vstack *t);
val val_array_get(fz_context *ctx, vstack *t, val array, int index);
val val_dict_get_key(fz_context *ctx, vstack *t, val array, int index);
val val_dict_get_value(fz_context *ctx, vstack *t, val array, int index);
const char *val_string(fz_context *ctx, vstack *t, val v);
const char *val_as_string(fz_context *ctx, vstack *t, val v);
const char *val_as_name(fz_context *ctx, vstack *t, val v);


#endif // VSTACK_H_
