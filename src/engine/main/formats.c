#include <stdio.h>
#include "formats.h"

// File format management
//
// It is common to omit file extensions in TeX ecosystem :)
// ... We need to guess them.

const char *exts_enc[]       = {".enc", NULL};
const char *exts_font_map[]  = {".map", NULL};
const char *exts_tfm[]       = {".tfm", NULL};
const char *exts_vf[]        = {".vf", NULL};
const char *exts_true_type[] = {".ttf", NULL};
const char *exts_type1[]     = {".pfb", NULL};
const char *exts_open_type[] = {".otf", NULL};
const char *exts_tex[]       = {".tex", NULL};
const char *exts_none[]      = {NULL};

// Return a null-terminated list of possible file extensions for "format"
const char **format_extensions(ttbc_file_format format)
{
    switch (format)
    {
        case TTBC_FILE_FORMAT_ENC:       return exts_enc;
        case TTBC_FILE_FORMAT_FONT_MAP:  return exts_font_map;
        case TTBC_FILE_FORMAT_TFM:       return exts_tfm;
        case TTBC_FILE_FORMAT_VF:        return exts_vf;
        case TTBC_FILE_FORMAT_TRUE_TYPE: return exts_true_type;
        case TTBC_FILE_FORMAT_TYPE1:     return exts_type1;
        case TTBC_FILE_FORMAT_OPEN_TYPE: return exts_open_type;
        case TTBC_FILE_FORMAT_TEX:       return exts_tex;
        default:                         return exts_none;
    }
}

#define CASE(x) case TTBC_FILE_FORMAT_##x: return #x

// Format to string for debugging purposes
const char *ttbc_file_format_to_string(ttbc_file_format format)
{
    switch (format)
    {
        CASE(AFM);
        CASE(BIB);
        CASE(BST);
        CASE(CMAP);
        CASE(CNF);
        CASE(ENC);
        CASE(FORMAT);
        CASE(FONT_MAP);
        CASE(MISC_FONTS);
        CASE(OFM);
        CASE(OPEN_TYPE);
        CASE(OVF);
        CASE(PICT);
        CASE(PK);
        CASE(PROGRAM_DATA);
        CASE(SFD);
        CASE(TECTONIC_PRIMARY);
        CASE(TEX);
        CASE(TEX_PS_HEADER);
        CASE(TFM);
        CASE(TRUE_TYPE);
        CASE(TYPE1);
        CASE(VF);
    }
    static char buf[80];
    sprintf(buf, "unknown format %d", format);
    return buf;
}
