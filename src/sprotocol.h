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

#ifndef SPROTOCOL_H
#define SPROTOCOL_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "myabort.h"

typedef struct channel_s channel_t;
typedef int file_id;

#define LOG 0

#define LEN(txt) (sizeof(txt)-1)
#define STR(X) #X
#define SSTR(X) STR(X)

#define pabort() \
  do { perror(__FILE__ ":" SSTR(__LINE__)); myabort(); } while(0)

#define mabort(...) \
  do { fprintf(stderr, "Aborting from " __FILE__ ":" SSTR(__LINE__) "\n" __VA_ARGS__); abort(); } while(0)

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)

/* QUERIES */

enum query {
  Q_OPEN = PACK('O','P','E','N'),
  Q_READ = PACK('R','E','A','D'),
  Q_WRIT = PACK('W','R','I','T'),
  Q_CLOS = PACK('C','L','O','S'),
  Q_SIZE = PACK('S','I','Z','E'),
  Q_SEEN = PACK('S','E','E','N'),
  Q_GPIC = PACK('G','P','I','C'),
  Q_SPIC = PACK('S','P','I','C'),
  Q_CHLD = PACK('C','H','L','D'),
};

struct pic_cache {
  int type, page;
  float bounds[4];
};

typedef struct {
  int time;
  enum query tag;
  union {
    struct {
      file_id fid;
      char *path, *mode;
    } open;
    struct {
      file_id fid;
      int pos, size;
    } read;
    struct {
      file_id fid;
      int pos, size;
      char *buf;
    } writ;
    struct {
      file_id fid;
    } clos;
    struct {
      file_id fid;
    } size;
    struct {
      file_id fid;
      int pos;
    } seen;
    struct {
      int fd;
      int pid;
    } chld;
    struct {
      char *path;
      int type, page;
    } gpic;
    struct {
      char *path;
      struct pic_cache cache;
    } spic;
  };
} query_t;

/* ANSWERS */

enum answer {
  A_DONE = PACK('D','O','N','E'),
  A_PASS = PACK('P','A','S','S'),
  A_SIZE = PACK('S','I','Z','E'),
  A_READ = PACK('R','E','A','D'),
  A_FORK = PACK('F','O','R','K'),
  A_OPEN = PACK('O','P','E','N'),
  A_GPIC = PACK('G','P','I','C'),
};

enum accs_answer {
  ACCS_PASS = 0,
  ACCS_OK   = 1,
  ACCS_ENOENT = 2,
  ACCS_EACCES = 3,
};

struct stat_time {
  uint32_t sec, nsec;
};

struct stat_answer {
  uint32_t dev, ino;
  uint32_t mode;
  uint32_t nlink;
  uint32_t uid, gid;
  uint32_t rdev;
  uint32_t size;
  uint32_t blksize, blocks;
  struct stat_time atime, ctime, mtime;
};

#define READ_FORK (-1)

typedef struct {
  enum answer tag;
  union {
    struct {
      int size;
    } size;
    struct {
      int size;
    } read;
    struct {
      int size;
    } open;
    struct {
      float bounds[4];
    } gpic;
  };
} answer_t;

/* "ASK" :P */

enum ask {
  C_FLSH = PACK('F','L','S','H'),
};

typedef struct {
  enum ask tag;
  union {
    struct {
      int pid;
    } term;
    struct {
      int fid, pos;
    } fenc;
    struct {
      int fid;
    } flsh;
  };
} ask_t;

/* Functions */

channel_t *channel_new(void);
void channel_free(channel_t *c);

bool channel_handshake(channel_t *c, int fd);
bool channel_has_pending_query(channel_t *t, int fd, int timeout);
bool channel_read_query(channel_t *t, int fd, query_t *r);
void channel_write_ask(channel_t *t, int fd, ask_t *a);
void channel_write_answer(channel_t *t, int fd, answer_t *a);
void *channel_get_buffer(channel_t *t, size_t n);
void channel_flush(channel_t *t, int fd);
void channel_reset(channel_t *t);

void log_query(FILE *f, query_t *q);
#endif /*!SPROTOCOL_H*/
