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

#ifndef MYDVI_OPCODES_H
#define MYDVI_OPCODES_H

enum dvi_opcode
{
  /* DVI op codes */
  SET_CHAR_0    =   0,
  SET_CHAR_1    =   1,
  /* etc. */
  SET_CHAR_127  = 127,
  SET1          = 128, /* Typesets its single operand between 128 and 255 */
  SET2          = 129, /* Typesets its single two byte unsigned operand */
  SET3          = 130, /* Typesets its single three byte unsigned operand */
  SET4          = 131, /* Typesets its single four byte unsigned operand */
  SET_RULE      = 132, /* Sets a rule of height param1(four bytes) and width param2(four bytes)
                          These are *signed*.  Nothing typeset for nonpositive values.
                          However, negative value *do* change current point */
  PUT1          = 133, /* Like SET1, but point doesn't change */
  PUT2          = 134, /* Like SET2 */
  PUT3          = 135, /* Like SET3 */
  PUT4          = 136, /* Like SET4 */
  PUT_RULE      = 137, /* Like SET_RULE */
  NOP           = 138,
  BOP           = 139, /* Followed by 10 four byte count registers (signed?).
                          Last parameter points to previous BOP (backward linked, first BOP has -1).
                          BOP clears stack and resets current point. */
  EOP           = 140,
  PUSH          = 141, /* Pushes h,v,w,x,y,z */
  POP           = 142, /* Opposite of push*/
  RIGHT1        = 143, /* Move right by one byte signed operand */
  RIGHT2        = 144, /* Move right by two byte signed operand */
  RIGHT3        = 145, /* Move right by three byte signed operand */
  RIGHT4        = 146, /* Move right by four byte signed operand */
  W0            = 147, /* Move right w */
  W1            = 148, /* w <- single byte signed operand.  Move right by same amount */
  W2            = 149, /* Same as W1 with two byte signed operand */
  W3            = 150, /* Three byte signed operand */
  W4            = 151, /* Four byte signed operand */
  X0            = 152, /* Move right x */
  X1            = 153, /* Like W1 */
  X2            = 154, /* Like W2 */
  X3            = 155, /* Like W3 */
  X4            = 156, /* Like W4 */
  DOWN1         = 157, /* Move down by one byte signed operand */
  DOWN2         = 158, /* Two byte signed operand */
  DOWN3         = 159, /* Three byte signed operand */
  DOWN4         = 160, /* Four byte signed operand */
  Y0            = 161, /* Move down by y */
  Y1            = 162, /* Move down by one byte signed operand, which replaces Y */
  Y2            = 163, /* Two byte signed operand */
  Y3            = 164, /* Three byte signed operand */
  Y4            = 165, /* Four byte signed operand */
  Z0            = 166, /* Like Y0, but use z */
  Z1            = 167, /* Like Y1 */
  Z2            = 168, /* Like Y2 */
  Z3            = 169, /* Like Y3 */
  Z4            = 170, /* Like Y4 */
  FNT_NUM_0     = 171, /* Switch to font 0 */
  FNT_NUM_1     = 172, /* Switch to font 1 */
  /* etc. */
  FNT_NUM_63    = 234, /* Switch to font 63 */
  FNT1          = 235, /* Switch to font described by single byte unsigned operand */
  FNT2          = 236, /* Switch to font described by two byte unsigned operand */
  FNT3          = 237, /* Three byte font descriptor */
  FNT4          = 238, /* Four byte operator (Knuth says signed, but what would be the point? */
  XXX1          = 239, /* Special.  Operand is one byte length.  Special follows immediately */
  XXX2          = 240, /* Two byte operand */
  XXX3          = 241, /* Three byte operand */
  XXX4          = 242, /* Four byte operand (Knuth says TeX uses only XXX1 and XXX4 */
  FNT_DEF1      = 243, /* One byte font number, four byte checksum, four byte magnified size (DVI units), four byte designed size, single byte directory length, single byte name length, followed by complete name (area+name) */
  FNT_DEF2      = 244, /* Same for two byte font number */
  FNT_DEF3      = 245, /* Same for three byte font number */
  FNT_DEF4      = 246, /* Four byte font number (Knuth says signed) */
  PRE           = 247, /* Preamble:
                          one byte DVI version (should be 2)
                          four byte unsigned numerator
                          four byte unsigned denominator -- one DVI unit = den/num*10^(-7) m
                          four byte magnification (multiplied by 1000)
                          one byte unsigned comment length followed by comment. */
  POST          = 248, /* Postamble- -- similar to preamble
                          four byte pointer to final bop
                          four byte numerator
                          four byte denominator
                          four byte mag
                          four byte maximum height (signed?)
                          four byte maximum width
                          two byte max stack depth required to process file
                          two byte number of pages */
  POST_POST     = 249, /* End of postamble
                          four byte pointer to POST command
                          Version byte (same as preamble)
                          Padded by four or more 223's to the end of the file. */
  PADDING       = 223,
  BEGIN_REFLECT = 250, /* TeX-XeT begin_reflect */
  END_REFLECT   = 251, /* TeX-XeT end_reflect */

  /* XeTeX ".xdv" codes */
  XDV_NATIVE_FONT_DEF = 252 , /* fontdef for native platform font */
  XDV_GLYPHS          = 253 , /* string of glyph IDs with X and Y positions */
  XDV_TEXT_GLYPHS     = 254 ,
  XDV_GLYPH_STRING    = 1000, /* TODO */

  PTEXDIR             = 255 , /* Ascii pTeX DIR command */
};

enum xdv_flags
{
  XDV_FLAG_VERTICAL = 0x0100,
  XDV_FLAG_COLORED  = 0x0200,
  XDV_FLAG_VARIATIONS = 0x0800,
  XDV_FLAG_EXTEND   = 0x1000,
  XDV_FLAG_SLANT    = 0x2000,
  XDV_FLAG_EMBOLDEN = 0x4000,
  XDV_FLAG_ALL      = XDV_FLAG_SLANT | XDV_FLAG_EMBOLDEN | XDV_FLAG_VARIATIONS |
                      XDV_FLAG_EXTEND | XDV_FLAG_COLORED | XDV_FLAG_VERTICAL,
};

static inline bool dvi_is_fontdef(uint8_t opcode)
{
  return
    (opcode >= FNT_DEF1 && opcode <= FNT_DEF4) ||
    (opcode == XDV_NATIVE_FONT_DEF);
}

#endif /*!MYDVI_OPCODES_H*/
