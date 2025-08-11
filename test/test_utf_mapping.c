#include "../src/utf_mapping.h"

// Test strings covering all corner cases
struct test_vec {
  const char *name, *comment, *input;
} tests[] = {
    {"test_ascii", "Basic ASCII (1-byte UTF-8, 1 UTF-16 code unit)", "Hello\n"},

    {"test_2byte",
     "Single 2-byte UTF-8 (é) - valid, should advance 1 UTF-16 unit",
     "caf\xc3\xa9\n"},

    {"test_3byte",
     "Single 3-byte UTF-8 (€) - valid, should advance 1 UTF-16 unit",
     "caf\xe2\x82\xac\n"},

    {"test_4byte",
     "Single 4-byte UTF-8 (🌈) - valid, should advance 2 UTF-16 units",
     "caf\xf0\x9f\x8c\x88\n"},

    {"test_mixed", "Mixed: 1-byte + 2-byte + 3-byte + 4-byte",
     "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x8c\x88\n"},

    {"test_invalid_continuation", "Invalid continuation byte (no leading byte)",
     "\x80\x81\x82\n"},

    {"test_overlong", "Overlong encoding (e.g., 2-byte for U+0000)",
     "\xC0\x80\xC1\xBF\n"},

    {"test_surrogate",
     "Surrogate pair in UTF-8 (invalid, should trigger error, U+D800)",
     "\xED\xA0\x80\n"},

    {"test_surrogate_pair", "Surrogate pair (U+D800 + U+DFFF)",
     "\xED\xA0\x80\xED\xAF\xBF\n"},

    {"test_nl_in_middle_2byte",
     "Line feed in the middle of a codepoint (should fail)",
     "ca\xE9\n"},  // \xE9 is continuation, but \xE9 alone is invalid

    {"test_nl_in_middle_3byte", "\\xE2\\x82 is incomplete 3-byte",
     "ca\xE2\x82\n"},

    {"test_nl_in_middle_4byte", "incomplete 4-byte", "ca\xF0\x90\x8C\n"},

    {"test_nl_after_surrogate",
     "Line feed after partial surrogate pair (high surrogate, then \\n)",
     "\xED\xA0\n"},

    {"test_trailing_continuation",
     "Trailing continuation bytes (no leading byte)", "\x80\x81\x82\x83\n"},

    {"test_end_mid_4byte", "incomplete 4-byte", "caf\xc3\xa9\xF0\x90\x8C"},

    {"test_middle_surrogate",
     "Pointing in middle of surrogate pair "
     "(should fail, U+D800 not followed by low surrogate)",
     "\xED\xA0\x80"},

    {NULL, NULL, NULL},
};

int fails = 0;
int last_offset = 0;

static void output_marker_at_offset(int offset)
{
  if (offset <= last_offset)
    abort();

  while (offset > last_offset && fails > 0)
  {
    putchar('_');
    last_offset += 1;
    fails -= 1;
  }
  fails = 0;

  while (offset > last_offset)
  {
    putchar(' ');
    last_offset += 1;
  }

  putchar('^');
  last_offset += 1;
}

int main()
{
  int counter = 0;
  for (struct test_vec *test = tests; test->name; test++)
  {
    printf("# Test %d. %s: %s\n\n", ++counter, test->name, test->comment);

    int len = strlen(test->input);
    const unsigned char *input = (void*)test->input;
    for (int i = 0; i < len; i++)
    {
      if (input[i] < 0x80 && input[i] > ' ')
        printf("| %c  ", input[i]);
      else
        printf("| %02X ", input[i]);
    }
    printf("|\n");

    fails = 0;
    last_offset = 0;

    for (int i = 0; i < len; i++)
    {
      int count = utf16_to_utf8_offset(test->input, test->input + len, i);
      if (count == -1)
        fails += 1;
      else
        output_marker_at_offset(count * 5 + 2);
    }

    while (fails > 0)
    {
      putchar('_');
      fails -= 1;
    }
    printf("\n");
    fflush(NULL);
    printf("\n");
  }

  return 0;
}
