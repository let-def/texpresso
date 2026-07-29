/*
 * Minimal fontconfig API shim for the wasm xetex build.
 *
 * xetex uses fontconfig only to ENUMERATE fonts (FcFontList) and read each
 * font's metadata; the actual font matching is xetex's own code. This shim
 * implements exactly that subset, backed by FreeType, driven by a manifest of
 * font paths (no cache, no fonts.conf, no directory scanning). It is API- (not
 * ABI-) compatible with the fontconfig xetex expects.
 *
 * Symbols implemented are only those xetex references:
 *   FcInit FcGetVersion FcConfigGetCurrent FcNameParse FcPatternDestroy
 *   FcObjectSetBuild FcObjectSetDestroy FcFontList
 *   FcPatternGetString FcPatternGetInteger
 */
#ifndef FCSHIM_FONTCONFIG_H
#define FCSHIM_FONTCONFIG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char FcChar8;
typedef int FcBool;
#define FcFalse 0
#define FcTrue 1

#define FC_VERSION 21201 /* pretend fontconfig 2.12.1 */

/* object (field) names xetex queries */
#define FC_FAMILY "family"
#define FC_STYLE "style"
#define FC_SLANT "slant"
#define FC_WEIGHT "weight"
#define FC_WIDTH "width"
#define FC_FILE "file"
#define FC_INDEX "index"
#define FC_FULLNAME "fullname"
#define FC_FONTFORMAT "fontformat"

typedef enum _FcResult {
  FcResultMatch,
  FcResultNoMatch,
  FcResultTypeMismatch,
  FcResultNoId,
  FcResultOutOfMemory
} FcResult;

typedef struct _FcConfig FcConfig;
typedef struct _FcPattern FcPattern;
typedef struct _FcObjectSet FcObjectSet;

/* xetex reads .nfont and .fonts[] directly, so this layout is load-bearing. */
typedef struct _FcFontSet {
  int nfont;
  int sfont;
  FcPattern **fonts;
} FcFontSet;

FcBool FcInit(void);
int FcGetVersion(void);
FcConfig *FcConfigGetCurrent(void);

FcPattern *FcNameParse(const FcChar8 *name);
void FcPatternDestroy(FcPattern *p);
FcResult FcPatternGetString(const FcPattern *p, const char *object, int n,
                            FcChar8 **s);
FcResult FcPatternGetInteger(const FcPattern *p, const char *object, int n,
                             int *i);

FcObjectSet *FcObjectSetBuild(const char *first, ...);
void FcObjectSetDestroy(FcObjectSet *os);

FcFontSet *FcFontList(FcConfig *config, FcPattern *p, FcObjectSet *os);

#ifdef __cplusplus
}
#endif

#endif /* FCSHIM_FONTCONFIG_H */
