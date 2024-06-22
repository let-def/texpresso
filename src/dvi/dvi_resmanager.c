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

#include <ft2build.h>
#include FT_FREETYPE_H
#include "mydvi.h"
#include "fz_util.h"
#include "../mupdf_compat.h"
#include <sys/wait.h>
#include <sys/file.h>
#include <unistd.h>
#include <errno.h>

typedef struct cell_dvi_font cell_dvi_font;
typedef struct cell_tex_enc cell_tex_enc;
typedef struct cell_pdf_doc cell_pdf_doc;
typedef struct cell_fz_font cell_fz_font;
typedef struct cell_image cell_image;

struct cell_dvi_font {
  dvi_font font;
  cell_dvi_font *next;
};

struct cell_tex_enc {
  const char *name;
  tex_enc *enc;
  cell_tex_enc *next;
};

struct cell_fz_font {
  const char *name;
  int index;
  fz_font *font;
  cell_fz_font *next;
};

struct cell_pdf_doc {
  const char *name;
  pdf_document *doc;
  cell_pdf_doc *next;
};

struct cell_image {
  const char *name;
  fz_image *img;
  cell_image *next;
};

struct dvi_resmanager {
  dvi_reshooks hooks;
  cell_dvi_font *first_dvi_font;
  cell_tex_enc  *first_tex_enc;
  cell_pdf_doc  *first_pdf_doc;
  cell_fz_font  *first_fz_font;
  cell_image    *first_image;
  tex_fontmap *map;
};

static void
default_hooks_free_env(fz_context *ctx, void *env)
{
  fz_free(ctx, env);
}

static fz_stream *
tectonic_hooks_open_file(fz_context *ctx, void *env, dvi_reskind kind, const char *name)
{
  char *path = NULL;
  bool free_path = 0;
  fprintf(stderr, "[dvi] loading %s\n", name);
  switch (kind)
  {
    case RES_PDF:
      if (name[0] == '/')
        path = (char*)name;
      else
      {
        char *root = env;
        int root_len = strlen(root);
        int name_len = strlen(name);
        int len = root_len + name_len;
        int need_slash = (root_len > 0 && root[root_len - 1] != '/');

        if (need_slash)
          len += 1;

        path = malloc(len + 1);
        if (!path) abort();
        free_path = 1;

        memcpy(path, root, root_len);
        if (need_slash)
        {
          path[root_len] = '/';
          memcpy(path + root_len + 1, name, name_len);
        }
        else
          memcpy(path + root_len, name, name_len);
        path[len] = 0;
      }
      break;

    case RES_FONT:
      if (name[0] == '/' || name[0] == '.')
      {
        path = (char *)name;
        break;
      }
    case RES_ENC:
    case RES_MAP:
    case RES_TFM:
    case RES_VF:
    {
      const char *ext0 = name;
      while (*ext0 && *ext0 != '.') ext0++;
      const char *exts[5] = {ext0, NULL};
      if (!*ext0)
      {
        switch (kind)
        {
          case RES_ENC:
            exts[0] = ".enc";
            break;
          case RES_MAP:
            exts[0] = ".map";
            break;
          case RES_TFM:
            exts[0] = ".tfm";
            break;
          case RES_VF:
            exts[0] = ".vf";
            break;
          case RES_FONT:
            exts[0] = ".pfb";
            exts[1] = ".otf";
            exts[2] = ".ttf";
            exts[3] = NULL;
            break;
          default:
            exts[0] = "";
        }
      }
      else
        exts[0] = "";

      for (const char **ext = exts; *ext; ++ext)
      {
        char command[1024];
        sprintf(command, "tectonic -X bundle cat %s%s", name, *ext);
        FILE *f = popen(command, "r");
        if (!f)
          abort();
        fz_stream *stream = fz_open_file_ptr_no_close(ctx, f);
        fz_buffer *buffer = fz_read_all(ctx, stream, 4096);
        fz_drop_stream(ctx, stream);
        if (pclose(f) != 0)
        {
          fz_drop_buffer(ctx, buffer);
          continue;
        }
        stream = fz_open_buffer(ctx, buffer);
        fz_drop_buffer(ctx, buffer);
        return stream;
      }
      return NULL;
    }
    break;

    default:
      abort();
  }

  fz_ptr(fz_stream, result);

  if (path == NULL)
  {
    fprintf(stderr, "dvi_resmanager_open_file(%s): no path found\n", name);
    return NULL;
  }

  fz_try(ctx)
  {
    result = fz_open_file(ctx, path);
  }
  fz_catch(ctx)
  {
    fz_warn(
      ctx,
      "dvi_resmanager_open_file(%s): %s",
      name,
      fz_caught_message(ctx)
    );
  };

  if (free_path)
    free(path);

  return result;
}

dvi_reshooks dvi_tectonic_hooks(fz_context *ctx, const char *document_dir)
{
  char *path = fz_strdup(ctx, document_dir ? document_dir : "");
  return (dvi_reshooks){
    .env = path,
    .free_env = default_hooks_free_env,
    .open_file = tectonic_hooks_open_file,
  };
}

struct bundle_server {
  char *document_dir;
  pid_t pid;
  FILE *o, *i, *lock;
};

static void my_flock(int fd, int flag)
{
  while (flock(fd, flag) == -1)
  {
    if (errno == EINTR) continue;
    perror("bundle_serve_hooks_cat: flock");
    abort();
  }
}

static fz_stream *
bundle_serve_hooks_cat(fz_context *ctx, struct bundle_server *env, const char *name)
{
  fz_stream *result = NULL;

  my_flock(fileno(env->lock), LOCK_EX);

  if (fwrite(name, strlen(name), 1, env->o) != 1)
  {
    fprintf(stderr, "bundle_serve_hooks_cat: cannot send request\n");
    goto release;
  }
  if (fwrite("\n", 1, 1, env->o) != 1)
  {
    fprintf(stderr, "bundle_serve_hooks_cat: cannot send newline\n");
    goto release;
  }
  if (fflush(env->o) != 0)
  {
    perror("bundle_serve_hooks_cat: fflush");
    goto release;
  }
  uint8_t answer[9];
  if (fread(answer, 9, 1, env->i) != 1)
  {
    fprintf(stderr, "bundle_serve_hooks_cat: cannot read answer\n");
    goto release;
  }

  switch (answer[0])
  {
    case 'C': case 'P': case 'E': 
    break;
    default:
    fprintf(stderr, "bundle_serve_hooks_cat: unknown response %C\n", answer[0]);
    abort();
  };

  uint64_t size =
    ((uint64_t)answer[1] << (0 * 8)) |
    ((uint64_t)answer[2] << (1 * 8)) |
    ((uint64_t)answer[3] << (2 * 8)) |
    ((uint64_t)answer[4] << (3 * 8)) |
    ((uint64_t)answer[5] << (4 * 8)) |
    ((uint64_t)answer[6] << (5 * 8)) |
    ((uint64_t)answer[7] << (6 * 8)) |
    ((uint64_t)answer[8] << (7 * 8));

  fz_buffer *buffer = fz_new_buffer(ctx, size);
  buffer->len = size;
  fprintf(stderr, "success code:%c size:%d\n", answer[0], (int)size);

  if (fread(buffer->data, size, 1, env->i) != 1)
  {
    fz_drop_buffer(ctx, buffer);
    fprintf(stderr, "bundle_serve_hooks_cat: cannot read data\n");
    goto release;
  }

  if (answer[0] == 'C')
    result = fz_open_buffer(ctx, buffer);
  else if (answer[0] == 'P')
    result = fz_open_file(ctx, fz_string_from_buffer(ctx, buffer));
  else
    fprintf(stderr, "bundle_serve_hooks_cat: error loading %s: %.*s\n",
            name, (int)size, buffer->data);
  fz_drop_buffer(ctx, buffer);

release:
  my_flock(fileno(env->lock), LOCK_UN);
  return result;
}

static fz_stream *
bundle_serve_hooks_open_file(fz_context *ctx, void *_env, dvi_reskind kind, const char *name)
{
  char *path = NULL;
  bool free_path = 0;
  fprintf(stderr, "[dvi] loading %s\n", name);
  bundle_server *env = _env;
  switch (kind)
  {
    case RES_PDF:
      if (name[0] == '/')
        path = (char*)name;
      else
      {
        const char *root = env->document_dir;
        int root_len = strlen(root);
        int name_len = strlen(name);
        int len = root_len + name_len;
        int need_slash = (root_len > 0 && root[root_len - 1] != '/');

        if (need_slash)
          len += 1;

        path = malloc(len + 1);
        if (!path) abort();
        free_path = 1;

        memcpy(path, root, root_len);
        if (need_slash)
        {
          path[root_len] = '/';
          memcpy(path + root_len + 1, name, name_len);
        }
        else
          memcpy(path + root_len, name, name_len);
        path[len] = 0;
      }
      break;

    case RES_FONT:
      if (name[0] == '/' || name[0] == '.' || fz_file_exists(ctx, name))
      {
        path = (char *)name;
        break;
      }
    case RES_ENC:
    case RES_MAP:
    case RES_TFM:
    case RES_VF:
    {
      const char *ext0 = name;
      while (*ext0 && *ext0 != '.') ext0++;
      const char *exts[5] = {ext0, NULL};
      if (!*ext0)
      {
        switch (kind)
        {
          case RES_ENC:
            exts[0] = ".enc";
            break;
          case RES_MAP:
            exts[0] = ".map";
            break;
          case RES_TFM:
            exts[0] = ".tfm";
            break;
          case RES_VF:
            exts[0] = ".vf";
            break;
          case RES_FONT:
            exts[0] = ".pfb";
            exts[1] = ".otf";
            exts[2] = ".ttf";
            exts[3] = NULL;
            break;
          default:
            exts[0] = "";
        }
      }
      else
        exts[0] = "";

      for (const char **ext = exts; *ext; ++ext)
      {
        char path[1024];
        sprintf(path, "%s%s", name, *ext);
        fz_stream *stream = bundle_serve_hooks_cat(ctx, env, path);
        if (stream)
          return stream;
      }
      return NULL;
    }
    break;

    default:
      abort();
  }

  fz_ptr(fz_stream, result);

  if (path == NULL)
  {
    fprintf(stderr, "dvi_resmanager_open_file(%s): no path found\n", name);
    return NULL;
  }

  fz_try(ctx)
  {
    result = fz_open_file(ctx, path);
  }
  fz_catch(ctx)
  {
    fz_warn(
      ctx,
      "dvi_resmanager_open_file(%s): %s",
      name,
      fz_caught_message(ctx)
    );
  };

  if (free_path)
    free(path);

  return result;
}

static void
bundle_serve_free_env(fz_context *ctx, void *_env)
{
  bundle_server *env = _env;

  if (fclose(env->i) != 0)
    perror("bundle_serve_free_env: fclose(i)");

  if (fclose(env->o) != 0)
    perror("bundle_serve_free_env: fclose(o)");

  int *dummy = 0;
  if (waitpid(env->pid, dummy, 0) != env->pid)
    perror("bundle_serve_free_env: waitpid");

  fz_free(ctx, env->document_dir);
  fz_free(ctx, env);
}

bundle_server *
bundle_server_start(fz_context *ctx,
                    const char *tectonic_path,
                    const char *document_dir)
{
  char buffer[4096];
  strcpy(buffer, tectonic_path);
  strcat(buffer, " -X bundle serve");

  int to_child[2], from_child[2];

  if (pipe(to_child) == -1)
  {
    perror("dvi_bundle_serve_hooks: pipe(to_child)");
    abort();
  }

  if (pipe(from_child) == -1)
  {
    perror("dvi_bundle_serve_hooks: pipe(to_child)");
    abort();
  }

  pid_t pid = fork();

  if (pid == -1)
  {
    perror("dvi_bundle_serve_hooks: fork");
    abort();
  }

  if (pid == 0)
  {
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    close(to_child[1]);
    close(from_child[0]);
    execlp(tectonic_path, "texpresso-tonic", "-X", "bundle", "serve", NULL);
    abort();
  }

  FILE *o = fdopen(to_child[1], "wb");
  if (!o)
  {
    perror("dvi_bundle_serve_hooks: fdopen");
    abort();
  }

  FILE *i = fdopen(from_child[0], "rb");
  if (!i)
  {
    perror("dvi_bundle_serve_hooks: fdopen");
    abort();
  }

  close(to_child[0]);
  close(from_child[1]);

  char *path = fz_strdup(ctx, document_dir ? document_dir : "");
  bundle_server *env = fz_malloc_struct(ctx, bundle_server);
  env->i = i;
  env->o = o;
  env->pid = pid;
  env->lock = tmpfile();
  if (!env->lock)
  {
    perror("bundle_server_start: tmpfile");
    abort();
  }
  env->document_dir = path;
  return env;
}

int bundle_server_input(bundle_server *server)
{
  return fileno(server->i);
}

int bundle_server_output(bundle_server *server)
{
  return fileno(server->o);
}

int bundle_server_lock(bundle_server *server)
{
  return fileno(server->lock);
}

dvi_reshooks bundle_server_hooks(bundle_server *env)
{
  return (dvi_reshooks){
    .env = env,
    .free_env = bundle_serve_free_env,
    .open_file = bundle_serve_hooks_open_file,
  };
}

void dvi_free_hooks(fz_context *ctx, const dvi_reshooks *hooks)
{
  if (hooks->free_env)
    hooks->free_env(ctx, hooks->env);
}


static fz_stream *dvi_resmanager_open_file(fz_context *ctx, dvi_resmanager *rm, dvi_reskind kind, const char *path)
{
  if (!rm->hooks.open_file)
    return NULL;

  return rm->hooks.open_file(ctx, rm->hooks.env, kind, path);
}

static void load_fontmap(fz_context *ctx, dvi_resmanager *rm)
{
  if (rm->map)
  {
    tex_fontmap_free(ctx, rm->map);
    rm->map = NULL;
  }

  fz_stream *stm[3];
  fz_var(stm);
  fz_try(ctx)
  {
    stm[0] = dvi_resmanager_open_file(ctx, rm, RES_MAP, "pdftex.map");
    stm[1] = dvi_resmanager_open_file(ctx, rm, RES_MAP, "kanjix.map");
    stm[2] = dvi_resmanager_open_file(ctx, rm, RES_MAP, "ckx.map");

    // printf(stm ? "FONT: loading fontmap\n" : "FONT: no fontmap\n");
    rm->map = tex_fontmap_load(ctx, stm, 3);
  }
  fz_always(ctx)
  {
    for (int i = 0; i < 3; ++i)
      if (stm[i])
        fz_drop_stream(ctx, stm[i]);
  }
  fz_try_rethrow(ctx);
}

dvi_resmanager *dvi_resmanager_new(fz_context *ctx, dvi_reshooks hooks)
{
  dvi_resmanager *rm = fz_malloc_struct(ctx, dvi_resmanager);
  rm->first_dvi_font = NULL;
  rm->first_tex_enc = NULL;
  rm->first_pdf_doc = NULL;
  rm->first_fz_font = NULL;
  rm->first_image = NULL;
  rm->hooks = hooks;

  load_fontmap(ctx, rm);

  return rm;
}

void dvi_resmanager_free(fz_context *ctx, dvi_resmanager *rm)
{
  dvi_free_hooks(ctx, &rm->hooks);

  if (rm->map)
  {
    tex_fontmap_free(ctx, rm->map);
    rm->map = NULL;
  }

  for (cell_dvi_font *cell = rm->first_dvi_font; cell; )
  {
    cell_dvi_font *next = cell->next;
    fz_free(ctx, (void*)cell->font.name);
    if (cell->font.tfm)
      tex_tfm_free(ctx, cell->font.tfm);
    if (cell->font.fz)
      fz_drop_font(ctx, cell->font.fz);
    fz_free(ctx, cell);
    cell = next;
  }
  for (cell_tex_enc *cell = rm->first_tex_enc; cell; )
  {
    cell_tex_enc *next = cell->next;
    fz_free(ctx, (void*)cell->name);
    if (cell->enc)
      tex_enc_free(ctx, cell->enc);
    fz_free(ctx, cell);
    cell = next;
  }
  for (cell_fz_font *cell = rm->first_fz_font; cell; )
  {
    cell_fz_font *next = cell->next;
    fz_free(ctx, (void*)cell->name);
    if (cell->font)
      fz_drop_font(ctx, cell->font);
    fz_free(ctx, cell);
    cell = next;
  }
  for (cell_pdf_doc *cell = rm->first_pdf_doc; cell; )
  {
    cell_pdf_doc *next = cell->next;
    fz_free(ctx, (void*)cell->name);
    if (cell->doc)
      pdf_drop_document(ctx, cell->doc);
    fz_free(ctx, cell);
    cell = next;
  }
  for (cell_image *cell = rm->first_image; cell; )
  {
    cell_image *next = cell->next;
    fz_free(ctx, (void*)cell->name);
    if (cell->img)
      fz_drop_image(ctx, cell->img);
    fz_free(ctx, cell);
    cell = next;
  }

  fz_free(ctx, rm);
}

static tex_enc *dvi_resmanager_get_tex_enc(fz_context *ctx, dvi_resmanager *rm, const char *name)
{
  for (cell_tex_enc *cell = rm->first_tex_enc; cell; cell = cell->next)
  {
    if (strcmp(name, cell->name) == 0)
      return cell->enc;
  }

  cell_tex_enc *cell = fz_malloc_struct(ctx, cell_tex_enc);
  cell->next = rm->first_tex_enc;
  rm->first_tex_enc = cell;
  cell->name = fz_strdup(ctx, name);

  fz_ptr(fz_stream, stm);
  fz_try(ctx)
  {
    stm = dvi_resmanager_open_file(ctx, rm, RES_ENC, name);
    if (stm)
      cell->enc = tex_enc_load(ctx, stm);
  }
  fz_always(ctx)
  {
    if (stm)
      fz_drop_stream(ctx, stm);
  }
  fz_catch(ctx)
  {
  }

  return cell->enc;
}

static fz_font *dvi_resmanager_get_fz_font(fz_context *ctx, dvi_resmanager *rm, const char *name, int len, int index)
{
  for (cell_fz_font *cell = rm->first_fz_font; cell; cell = cell->next)
  {
    if (strncmp(name, cell->name, len) == 0 &&
        cell->name[len] == 0 &&
        cell->index == index)
      return cell->font;
  }

  fz_ptr(cell_fz_font, cell);
  fz_ptr(char, cell_name);
  fz_ptr(fz_stream, stm);
  fz_ptr(fz_buffer, buf);

  fz_try(ctx)
  {
    cell = fz_malloc_struct(ctx, cell_fz_font);
    cell_name = dtx_strndup(ctx, name, len);
    cell->next = rm->first_fz_font;
    cell->name = cell_name;
    cell->index = index;

    fprintf(stderr, "dvi_resmanager_get_fz_font: loading font %s\n", cell_name);

    stm = dvi_resmanager_open_file(ctx, rm, RES_FONT, cell_name);

    if (stm)
    {
      buf = fz_read_all(ctx, stm, 16384);
      cell->font = fz_new_font_from_buffer(ctx, NULL, buf, index, 0);
    }

    if (cell->font)
    {
      FT_Face face = fz_font_ft_face(ctx, cell->font);
      if (face)
      {
        int count = face->num_charmaps;
        for (int i = 0; i < count; ++i)
        {
          FT_CharMap cm = face->charmaps[i];
          if (cm->platform_id == 7 && cm->encoding_id == 2)
            FT_Set_Charmap(face, cm);
        }
      }
    }
  }
  fz_always(ctx)
  {
    if (stm)
      fz_drop_stream(ctx, stm);
    if (buf)
      fz_drop_buffer(ctx, buf);
  }
  fz_catch(ctx)
  {
    if (cell)
      fz_free(ctx, cell);
    if (cell_name)
      fz_free(ctx, cell_name);
    fz_rethrow(ctx);
  }
  rm->first_fz_font = cell;

  return cell->font;
}

dvi_font *dvi_resmanager_get_tex_font(fz_context *ctx, dvi_resmanager *rm, const char *name, int len)
{
  for (cell_dvi_font *cell = rm->first_dvi_font; cell; cell = cell->next)
  {
    if (strncmp(name, cell->font.name, len) == 0 && cell->font.name[len] == 0)
      return &cell->font;
  }

  cell_dvi_font *cell = fz_malloc_struct(ctx, cell_dvi_font);
  char *font_name = dtx_strndup(ctx, name, len);
  cell->font.name = font_name;
  font_name[len] = 0;
  cell->next = rm->first_dvi_font;
  rm->first_dvi_font = cell;

  tex_fontmap_entry *e = tex_fontmap_lookup(rm->map, cell->font.name);

  if (e && e->font_file_name)
  {
    cell->font.fz = fz_keep_font(ctx, dvi_resmanager_get_fz_font(ctx, rm, e->font_file_name, strlen(e->font_file_name), 0));
    if (e->enc_file_name)
      cell->font.enc = dvi_resmanager_get_tex_enc(ctx, rm, e->enc_file_name);
  }

  fz_ptr(fz_stream, stm);

  /* Load TFM */
  stm = NULL;
  fz_try(ctx)
  {
    stm = dvi_resmanager_open_file(ctx, rm, RES_TFM, cell->font.name);
    if (stm)
      cell->font.tfm = tex_tfm_load(ctx, stm);
  }
  fz_always(ctx)
  {
    if (stm)
      fz_drop_stream(ctx, stm);
    stm = NULL;
  }
  fz_catch(ctx)
  {
    fz_warn(ctx,
        "dvi_resmanager_get_tex_font(%s): "
        "could not load TFM file, ignoring metrics (error %s)",
        name,
        fz_caught_message(ctx)
        );
  }

  /* Load VF */
  stm = NULL;
  fz_try(ctx)
  {
    stm = dvi_resmanager_open_file(ctx, rm, RES_VF, cell->font.name);
    if (stm)
      cell->font.vf = tex_vf_load(ctx, rm, stm);
  }
  fz_always(ctx)
  {
    if (stm)
      fz_drop_stream(ctx, stm);
  }
  fz_catch(ctx)
  {
    fz_warn(ctx,
      "dvi_resmanager_get_tex_font(%s): "
      "could not load VF file, skipping font (error %s)",
      name,
      fz_caught_message(ctx)
    );
  }

  if (!cell->font.vf && !cell->font.fz)
  {
    fz_warn(
      ctx,
      "dvi_resmanager_get_tex_font(%s): "
      "no font file nor VF file found",
      cell->font.name
    );
  }

  return &cell->font;
}

fz_font *dvi_resmanager_get_xdv_font(fz_context *ctx, dvi_resmanager *rm, const char *name, int len, int index)
{
  return dvi_resmanager_get_fz_font(ctx, rm, name, len, index);
}

void dvi_resmanager_invalidate(fz_context *ctx, dvi_resmanager *rm, dvi_reskind kind, const char *name)
{
  switch (kind)
  {
    case RES_PDF:
      for (cell_pdf_doc **cell = &rm->first_pdf_doc; *cell; cell = &(*cell)->next)
      {
        if (strcmp(name, (*cell)->name) != 0) continue;
        fz_free(ctx, (void*)(*cell)->name);
        pdf_drop_document(ctx, (*cell)->doc);
        cell_pdf_doc *next = (*cell)->next;
        fz_free(ctx, *cell);
        *cell = next;
      }
      break;

    case RES_ENC:
      for (cell_tex_enc **cell = &rm->first_tex_enc; *cell; cell = &(*cell)->next)
      {
        if (strcmp(name, (*cell)->name) != 0) continue;
        fz_free(ctx, (void*)(*cell)->name);
        tex_enc_free(ctx, (*cell)->enc);
        cell_tex_enc *next = (*cell)->next;
        fz_free(ctx, *cell);
        *cell = next;
      }
      break;

    case RES_MAP:
      abort();
      break;

    case RES_TFM:
    case RES_VF:
      for (cell_dvi_font **cell = &rm->first_dvi_font; *cell; cell = &(*cell)->next)
      {
        if (strcmp(name, (*cell)->font.name) != 0) continue;
        fz_free(ctx, (void*)(*cell)->font.name);
        if ((*cell)->font.tfm)
          tex_tfm_free(ctx, (*cell)->font.tfm);
        if ((*cell)->font.fz)
          fz_drop_font(ctx, (*cell)->font.fz);
        cell_dvi_font *next = (*cell)->next;
        fz_free(ctx, *cell);
        *cell = next;
      }
      break;

    case RES_FONT:
      for (cell_fz_font **cell = &rm->first_fz_font; *cell; cell = &(*cell)->next)
      {
        if (strcmp(name, (*cell)->name) != 0) continue;
        fz_free(ctx, (void*)(*cell)->name);
        if ((*cell)->font)
          fz_drop_font(ctx, (*cell)->font);
        cell_fz_font *next = (*cell)->next;
        fz_free(ctx, *cell);
        *cell = next;
      }
      break;

    default:
      abort();
  }
}

pdf_document *dvi_resmanager_get_pdf(fz_context *ctx, dvi_resmanager *rm, const char *filename)
{
  for (cell_pdf_doc *cell = rm->first_pdf_doc; cell; cell = cell->next)
    if (strcmp(filename, cell->name) == 0)
      return cell->doc;

  fz_ptr(cell_pdf_doc, cell);
  fz_ptr(char, pname);
  fz_ptr(fz_stream, stm);

  fz_try(ctx)
  {
    cell = fz_malloc_struct(ctx, cell_pdf_doc);
    pname = fz_strdup(ctx, filename);
    cell->name = pname;
    cell->next = rm->first_pdf_doc;
    stm = dvi_resmanager_open_file(ctx, rm, RES_PDF, pname);
    if (stm)
      cell->doc = pdf_open_document_with_stream(ctx, stm);
  }
  fz_always(ctx)
  {
    if (stm)
      fz_drop_stream(ctx, stm);
  }
  fz_catch(ctx)
  {
    if (cell)
      fz_free(ctx, cell);
    if (pname)
      fz_free(ctx, pname);
    fz_rethrow(ctx);
  }

  rm->first_pdf_doc = cell;

  return cell->doc;
}

fz_image *dvi_resmanager_get_img(fz_context *ctx, dvi_resmanager *rm, const char *filename)
{
  for (cell_image *cell = rm->first_image; cell; cell = cell->next)
    if (strcmp(filename, cell->name) == 0)
      return cell->img;

  fz_ptr(cell_image, cell);
  fz_ptr(char, pname);

  fz_try(ctx)
  {
    cell = fz_malloc_struct(ctx, cell_image);
    pname = fz_strdup(ctx, filename);
    cell->name = pname;
    cell->next = rm->first_image;
    cell->img = fz_new_image_from_file(ctx, filename);
  }
  fz_catch(ctx)
  {
    if (cell)
      fz_free(ctx, cell);
    if (pname)
      fz_free(ctx, pname);
    fz_rethrow(ctx);
  }

  rm->first_image = cell;

  return cell->img;
}
