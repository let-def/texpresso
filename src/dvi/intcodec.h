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

#ifndef __INTCODEC_H__
#define __INTCODEC_H__

#include <stdint.h>
#include <stdlib.h>
#include "fixed.h"

static uint8_t decode_u8(const uint8_t *buf)
{ return buf[0]; }

static uint16_t decode_u16(const uint8_t *buf)
{ return (buf[0] << 8) | (buf[1]); }

static uint32_t decode_u24(const uint8_t *buf)
{ return (buf[0] << 16) | (buf[1] << 8) | (buf[2]); }

static uint32_t decode_u32(const uint8_t *buf)
{ return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3]); }

static int8_t decode_s8(const uint8_t *buf)
{ return ((int8_t*)buf)[0]; }

static int16_t decode_s16(const uint8_t *buf)
{ return (decode_s8(buf) << 8) | (buf[1]); }

static int32_t decode_s24(const uint8_t *buf)
{ return (decode_s8(buf) << 16) | (buf[1] << 8) | (buf[2]); }

static int32_t decode_s32(const uint8_t *buf)
{ return (decode_s8(buf) << 24) | (buf[1] << 16) | (buf[2] << 8) | (buf[3]); }

static fixed_t decode_fixed(const uint8_t *buf)
{ return fixed_make(decode_s32(buf)); }

static int32_t decode_sB(const uint8_t *buf, int n)
{
    switch (n)
    {
        case 1: return decode_s8(buf);
        case 2: return decode_s16(buf);
        case 3: return decode_s24(buf);
        case 4: return decode_s32(buf);
        default: abort();
    }
}

static uint32_t decode_uB(const uint8_t *buf, int n)
{
    switch (n)
    {
        case 1: return decode_u8(buf);
        case 2: return decode_u16(buf);
        case 3: return decode_u24(buf);
        case 4: return decode_u32(buf);
        default: abort();
    }
}

static uint8_t read_u8(const uint8_t **buf)
{
    uint8_t result = decode_u8(*buf);
    *buf += 1;
    return result;
}

static uint16_t read_u16(const uint8_t **buf)
{
    uint16_t result = decode_u16(*buf);
    *buf += 2;
    return result;
}

static uint32_t read_u24(const uint8_t **buf)
{
    uint32_t result = decode_u24(*buf);
    *buf += 3;
    return result;
}

static uint32_t read_u32(const uint8_t **buf)
{
    uint32_t result = decode_u32(*buf);
    *buf += 4;
    return result;
}

static int8_t read_s8(const uint8_t **buf)
{
    int8_t result = decode_s8(*buf);
    *buf += 1;
    return result;
}

static int16_t read_s16(const uint8_t **buf)
{
    int16_t result = decode_s16(*buf);
    *buf += 2;
    return result;
}

static int32_t read_s24(const uint8_t **buf)
{
    int32_t result = decode_s24(*buf);
    *buf += 3;
    return result;
}

static int32_t read_s32(const uint8_t **buf)
{
    int32_t result = decode_s32(*buf);
    *buf += 4;
    return result;
}

static fixed_t read_fixed(const uint8_t **buf)
{
    fixed_t result = decode_fixed(*buf);
    *buf += 4;
    return result;
}

static int32_t read_sB(const uint8_t **buf, int n)
{
    int32_t result = decode_sB(*buf, n);
    *buf += n;
    return result;
}

static uint32_t read_uB(const uint8_t **buf, int n)
{
    uint32_t result = decode_uB(*buf, n);
    *buf += n;
    return result;
}

#endif /*__INTCODEC_H__*/
