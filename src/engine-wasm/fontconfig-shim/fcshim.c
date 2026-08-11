/*
 * Minimal fontconfig shim backed by FreeType (see fontconfig/fontconfig.h).
 *
 * FcFontList enumerates the font files listed in a manifest (one path per line;
 * path from $TEXPRESSO_FONT_MANIFEST, default "texpresso-fonts.lst"), reading
 * each face's metadata with FreeType and returning it in fontconfig's shape.
 * No cache, no fonts.conf, no directory scanning — the host/VFS decides which
 * fonts exist by way of the manifest.
 */
#include "fontconfig/fontconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H

/* ---- pattern: object name -> ordered list of string/int values ---- */
typedef struct { int is_int; int i; FcChar8 *s; } Val;
typedef struct { char *object; Val *vals; int n, cap; } Elt;
struct _FcPattern { Elt *elts; int n, cap; };
struct _FcObjectSet { int dummy; };
struct _FcConfig { int dummy; };

static FcPattern *pat_new(void) { return calloc(1, sizeof(FcPattern)); }

static Elt *pat_elt(FcPattern *p, const char *object) {
  for (int i = 0; i < p->n; i++)
    if (strcmp(p->elts[i].object, object) == 0) return &p->elts[i];
  if (p->n == p->cap) {
    p->cap = p->cap ? p->cap * 2 : 8;
    p->elts = realloc(p->elts, (size_t)p->cap * sizeof(Elt));
  }
  Elt *e = &p->elts[p->n++];
  memset(e, 0, sizeof *e);
  e->object = strdup(object);
  return e;
}

static void elt_push(Elt *e, Val v) {
  if (e->n == e->cap) {
    e->cap = e->cap ? e->cap * 2 : 4;
    e->vals = realloc(e->vals, (size_t)e->cap * sizeof(Val));
  }
  e->vals[e->n++] = v;
}

static void add_str(FcPattern *p, const char *object, const char *s) {
  if (!s || !*s) return;
  Val v = {0, 0, (FcChar8 *)strdup(s)};
  elt_push(pat_elt(p, object), v);
}
static void add_int(FcPattern *p, const char *object, int i) {
  Val v = {1, i, NULL};
  elt_push(pat_elt(p, object), v);
}

/* ---- OS/2 -> fontconfig weight/width scale ---- */
static int fc_weight(int os2) {
  static const int in[] = {100, 200, 300, 350, 380, 400, 500, 600, 700, 800, 900};
  static const int out[] = {0, 40, 50, 55, 75, 80, 100, 180, 200, 205, 210};
  int best = 80;
  int bd = 1 << 30;
  for (unsigned k = 0; k < sizeof(in) / sizeof(in[0]); k++) {
    int d = os2 > in[k] ? os2 - in[k] : in[k] - os2;
    if (d < bd) { bd = d; best = out[k]; }
  }
  return best;
}
static int fc_width(int os2) {
  static const int out[] = {50, 50, 63, 75, 87, 100, 113, 125, 150, 200};
  if (os2 < 1) os2 = 5;
  if (os2 > 9) os2 = 9;
  return out[os2];
}

/* ---- FreeType library (lazy) ---- */
static FT_Library g_ft;
static int ft_ready(void) {
  if (!g_ft && FT_Init_FreeType(&g_ft)) return 0;
  return 1;
}

/* full font name from SFNT name table (id 4), else family+style */
static void add_fullname(FcPattern *p, FT_Face f) {
  FT_UInt cnt = FT_Get_Sfnt_Name_Count(f);
  for (FT_UInt i = 0; i < cnt; i++) {
    FT_SfntName nm;
    if (FT_Get_Sfnt_Name(f, i, &nm)) continue;
    if (nm.name_id != TT_NAME_ID_FULL_NAME) continue;
    /* accept ASCII-ish (Mac Roman or UTF-16BE) English entries */
    if (nm.platform_id == TT_PLATFORM_MACINTOSH) {
      char buf[256];
      FT_UInt n = nm.string_len < 255 ? nm.string_len : 255;
      memcpy(buf, nm.string, n);
      buf[n] = 0;
      add_str(p, FC_FULLNAME, buf);
      return;
    }
    if (nm.platform_id == TT_PLATFORM_MICROSOFT) {
      char buf[256];
      FT_UInt n = 0;
      for (FT_UInt j = 1; j < nm.string_len && n < 255; j += 2)
        buf[n++] = (char)nm.string[j]; /* strip the UTF-16BE high byte */
      buf[n] = 0;
      add_str(p, FC_FULLNAME, buf);
      return;
    }
  }
  /* fallback */
  char buf[256];
  snprintf(buf, sizeof buf, "%s %s", f->family_name ? f->family_name : "",
           f->style_name ? f->style_name : "");
  add_str(p, FC_FULLNAME, buf);
}

static void add_face(FcFontSet *fs, const char *path, int index, FT_Face f) {
  FcPattern *p = pat_new();
  add_str(p, FC_FAMILY, f->family_name);
  add_str(p, FC_STYLE, f->style_name);
  add_fullname(p, f);
  add_str(p, FC_FILE, path);
  add_int(p, FC_INDEX, index);

  int weight = 80, width = 100;
  TT_OS2 *os2 = (TT_OS2 *)FT_Get_Sfnt_Table(f, FT_SFNT_OS2);
  if (os2 && os2->version != 0xFFFF) {
    weight = fc_weight(os2->usWeightClass);
    width = fc_width(os2->usWidthClass);
  } else if (f->style_flags & FT_STYLE_FLAG_BOLD) {
    weight = 200;
  }
  add_int(p, FC_WEIGHT, weight);
  add_int(p, FC_WIDTH, width);
  add_int(p, FC_SLANT, (f->style_flags & FT_STYLE_FLAG_ITALIC) ? 100 : 0);
  add_str(p, FC_FONTFORMAT, FT_IS_SFNT(f) ? "TrueType" : "Type 1");

  if (fs->nfont == fs->sfont) {
    fs->sfont = fs->sfont ? fs->sfont * 2 : 64;
    fs->fonts = realloc(fs->fonts, (size_t)fs->sfont * sizeof(FcPattern *));
  }
  fs->fonts[fs->nfont++] = p;
}

static void enumerate(FcFontSet *fs, const char *path) {
  if (!ft_ready()) return;
  FT_Face probe;
  if (FT_New_Face(g_ft, path, -1, &probe)) return;
  long nfaces = probe->num_faces;
  FT_Done_Face(probe);
  for (long i = 0; i < nfaces; i++) {
    FT_Face f;
    if (FT_New_Face(g_ft, path, i, &f)) continue;
    if (f->family_name) add_face(fs, path, (int)i, f);
    FT_Done_Face(f);
  }
}

/* ---- public API ---- */
FcBool FcInit(void) { return FcTrue; }
int FcGetVersion(void) { return FC_VERSION; }

FcConfig *FcConfigGetCurrent(void) {
  static FcConfig cfg;
  return &cfg;
}

FcPattern *FcNameParse(const FcChar8 *name) {
  (void)name; /* xetex passes ":outline=true"; we enumerate all outlines */
  return pat_new();
}

void FcPatternDestroy(FcPattern *p) {
  if (!p) return;
  for (int i = 0; i < p->n; i++) {
    for (int j = 0; j < p->elts[i].n; j++) free(p->elts[i].vals[j].s);
    free(p->elts[i].vals);
    free(p->elts[i].object);
  }
  free(p->elts);
  free(p);
}

FcResult FcPatternGetString(const FcPattern *p, const char *object, int n,
                            FcChar8 **s) {
  for (int i = 0; i < p->n; i++)
    if (strcmp(p->elts[i].object, object) == 0) {
      if (n < p->elts[i].n && !p->elts[i].vals[n].is_int) {
        *s = p->elts[i].vals[n].s;
        return FcResultMatch;
      }
      return FcResultNoMatch;
    }
  return FcResultNoMatch;
}

FcResult FcPatternGetInteger(const FcPattern *p, const char *object, int n,
                             int *out) {
  for (int i = 0; i < p->n; i++)
    if (strcmp(p->elts[i].object, object) == 0) {
      if (n < p->elts[i].n && p->elts[i].vals[n].is_int) {
        *out = p->elts[i].vals[n].i;
        return FcResultMatch;
      }
      return FcResultNoMatch;
    }
  return FcResultNoMatch;
}

FcObjectSet *FcObjectSetBuild(const char *first, ...) {
  (void)first; /* we always populate every field, so the requested set is moot */
  va_list ap;
  va_start(ap, first);
  while (va_arg(ap, const char *)) {}
  va_end(ap);
  return calloc(1, sizeof(FcObjectSet));
}
void FcObjectSetDestroy(FcObjectSet *os) { free(os); }

FcFontSet *FcFontList(FcConfig *config, FcPattern *p, FcObjectSet *os) {
  (void)config; (void)p; (void)os;
  FcFontSet *fs = calloc(1, sizeof(FcFontSet));

  const char *manifest = getenv("TEXPRESSO_FONT_MANIFEST");
  if (!manifest) manifest = "texpresso-fonts.lst";
  FILE *fp = fopen(manifest, "r");
  if (!fp) {
    fprintf(stderr, "fcshim: no font manifest '%s' (0 fonts)\n", manifest);
    return fs;
  }
  char line[4096];
  while (fgets(line, sizeof line, fp)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = 0;
    if (!len || line[0] == '#') continue;
    enumerate(fs, line);
  }
  fclose(fp);
  fprintf(stderr, "fcshim: enumerated %d face(s) from '%s'\n", fs->nfont,
          manifest);
  return fs;
}
