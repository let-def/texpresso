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

#include "logo.h"
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi.h"

// move logo blob to separate file
#include "logo_blob.inc"

// void *qoi_decode(const void *data, int size, qoi_desc *desc, int channels)

SDL_Surface *texpresso_logo(void)
{
  qoi_desc desc;
  int channels = 4;
  void *pixels = qoi_decode(logo, logo_len, &desc, channels);

  if (!pixels)
    abort();

  return SDL_CreateRGBSurfaceWithFormatFrom(pixels, desc.width,
      desc.height, 8 * channels, desc.width * channels, SDL_PIXELFORMAT_RGBA32);
}

