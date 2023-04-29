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

#ifndef FIXED_H
#define FIXED_H

#include <stdint.h>

#define K_PT2IN (1.0/72.27)
#define K_PT2BP (K_PT2IN*72.0)
#define K_PT2CM (K_PT2IN*2.54)
#define K_PT2MM (K_PT2CM*10.0)
#define K_PT2PC (1.0/12.0)
#define K_PT2DD (1157.0/1238.0)
#define K_PT2CC (K_PT2DD/12.0)
#define K_PT2SP (65536.0)

typedef struct {
    int32_t value;
} fixed_t;

static inline fixed_t fixed_make(int32_t repr);
static inline int fixed_compare(fixed_t a, fixed_t b);
static inline double fixed_double(fixed_t t);
static inline fixed_t fixed_mul(fixed_t a, fixed_t b);
static inline fixed_t fixed_div(fixed_t a, fixed_t b);

static inline fixed_t fixed_make(int32_t repr)
{
    return (fixed_t){.value = repr};
}

static inline int fixed_compare(fixed_t a, fixed_t b)
{
    if (a.value == b.value)
        return 0;
    else if (a.value < b.value)
        return -1;
    else
        return 1;
}

static inline double fixed_double(fixed_t t)
{
    return (double)t.value / (1 << 20);
}

static inline fixed_t fixed_mul(fixed_t a, fixed_t b)
{
    int64_t v = (int64_t)a.value * (int64_t)b.value;
    return fixed_make(v >> 20);
}

static inline fixed_t fixed_div(fixed_t a, fixed_t b)
{
  int64_t v = ((int64_t)a.value << 20) / (int64_t)b.value;
  return fixed_make(v);
  // double d = fixed_double(a) / fixed_double(b);
  // return fixed_make(d * (1 << 20));
}

#endif /*!FIXED*/
