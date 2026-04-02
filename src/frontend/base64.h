#ifndef BASE64_H_
#define BASE64_H_

// Decode base64 in-place. Returns decoded length, or -1 on error.
static int base64_decode(unsigned char *data, int len)
{
  static const unsigned char table[256] = {
    ['A']=0,  ['B']=1,  ['C']=2,  ['D']=3,  ['E']=4,  ['F']=5,  ['G']=6,
    ['H']=7,  ['I']=8,  ['J']=9,  ['K']=10, ['L']=11, ['M']=12, ['N']=13,
    ['O']=14, ['P']=15, ['Q']=16, ['R']=17, ['S']=18, ['T']=19, ['U']=20,
    ['V']=21, ['W']=22, ['X']=23, ['Y']=24, ['Z']=25,
    ['a']=26, ['b']=27, ['c']=28, ['d']=29, ['e']=30, ['f']=31, ['g']=32,
    ['h']=33, ['i']=34, ['j']=35, ['k']=36, ['l']=37, ['m']=38, ['n']=39,
    ['o']=40, ['p']=41, ['q']=42, ['r']=43, ['s']=44, ['t']=45, ['u']=46,
    ['v']=47, ['w']=48, ['x']=49, ['y']=50, ['z']=51,
    ['0']=52, ['1']=53, ['2']=54, ['3']=55, ['4']=56,
    ['5']=57, ['6']=58, ['7']=59, ['8']=60, ['9']=61,
    ['+']=62, ['/']=63,
  };

  // Strip padding
  while (len > 0 && data[len - 1] == '=')
    len--;

  int out = 0;
  int buf = 0, bits = 0;
  for (int i = 0; i < len; i++)
  {
    unsigned char c = data[i];
    if (c == '\n' || c == '\r' || c == ' ')
      continue;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '+' || c == '/'))
      return -1;
    buf = (buf << 6) | table[c];
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      data[out++] = (buf >> bits) & 0xFF;
    }
  }
  return out;
}

#endif // BASE64_H_
