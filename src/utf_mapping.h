#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Move the pointer `start` to an UTF-8 encoded string ending at `end` forward
// by `count` UTF-16 code units.
// Return NULL if a fatal error was encountered (invalid encoding or `count`
// points in the middle of a surrogate pair).
// LSP ranges
static const void *move_utf8_by_utf16_codeunits(const void *start,
                                                const void *end,
                                                int count)
{
  if (start >= end)
  {
    if (count == 0)
      return start;
    return NULL;
  }

  const unsigned char *p = start;
  int warned_continuation = 0;

  while (count > 0)
  {
    unsigned char b = *p;

    // 1-byte UTF-8: ASCII char
    if (b < 0x80)
    {

      // Error: reached the end of the line
      if (b == '\n')
      {
        fprintf(stderr,
                "[error] Invalid UTF-16 range: "
                "pointing past end of line\n");
        return NULL;
      }

      // Count as single UTF-16 code unit
      count -= 1;
      p += 1;

      // Out of bounds
      if ((void*)p > end)
        return NULL;
    }

    // 1-byte: continuation (warn and ignore)
    else if (b < 0xC0)
    {
      if (!warned_continuation)
      {
        fprintf(stderr,
                "[warning] UTF-16 range: "
                "unexpected continuation byte\n");
        warned_continuation = 1;
      }
      p += 1;
    }

    // 2-byte UTF-8, 1 UTF-16 code unit
    else if (b < 0xE0)
    {
      count -= 1;
      p += 2;
      if ((void*)p <= end && p[-1] == '\n')
        goto invalid_nl;
    }

    // 3-byte UTF-8, 1 UTF-16 code unit
    else if (b < 0xF0)
    {
      count -= 1;
      p += 3;
      if ((void*)p <= end && (p[-1] == '\n' || p[-2] == '\n'))
        goto invalid_nl;
    }

    // 4-byte UTF-8 encodings: translate to a surrogate pair, needing two
    // UTF-16 code units
    else
    {
      p += 4;
      count -= 2;
      if ((void*)p <= end && (p[-1] == '\n' || p[-2] == '\n' || p[-3] == '\n'))
        goto invalid_nl;
    }
  }

  // At this point, count is:
  // - exactly 0 if the traversal succeeded
  // - (-1) if the offset given pointed in the middle of a surrogate pair
  // - >0 if the offset is pointing past the end of the buffer
  //
  if (count > 0 || (void*)p > end)
  {
    fprintf(stderr,
            "[error] Invalid UTF-16 range: "
            "pointing past end of buffer\n");
    return NULL;
  }

  if (count < 0)
  {
    fprintf(stderr,
            "[error] Invalid UTF-16 range: "
            "pointing in the middle of a surrogate pair\n");
    return NULL;
  }

  return p;

invalid_nl:
  fprintf(stderr,
          "[error] Broken UTF-8 encoding: "
          "line return in the middle of a codepoint\n");
  return NULL;
}

static int utf16_to_utf8_offset(const void *p, const void *end, size_t utf16_index)
{
  const void *p2 = move_utf8_by_utf16_codeunits(p, end, utf16_index);

  if (p2 && p2 <= end)
    return p2 - p;
  else
    return -1;
}
