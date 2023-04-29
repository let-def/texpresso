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

#define mabort() \
  do { fputs("Aborting from " __FILE__ ":" SSTR(__LINE__) "\n", stderr); myabort(); } while(0)

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)

/* QUERIES */

enum query {
  Q_OPEN = PACK('O','P','E','N'),
  Q_READ = PACK('R','E','A','D'),
  Q_WRIT = PACK('W','R','I','T'),
  Q_CLOS = PACK('C','L','O','S'),
  Q_SIZE = PACK('S','I','Z','E'),
  Q_SEEN = PACK('S','E','E','N'),
  Q_CHLD = PACK('C','H','L','D'),
  Q_BACK = PACK('B','A','C','K'),
  Q_ACCS = PACK('A','C','C','S'),
  Q_STAT = PACK('S','T','A','T'),
};

enum accs_flag {
  ACCS_R = 1,
  ACCS_W = 2,
  ACCS_X = 4,
  ACCS_F = 8
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
      int pid;
    } chld;
    struct {
      int pid, cid, exitcode;
    } back;
    struct {
      char *path;
      int flags;
    } accs;
    struct {
      char * path;
    } stat;
  };
} query_t;

/* ANSWERS */

enum answer {
  A_DONE = PACK('D','O','N','E'),
  A_PASS = PACK('P','A','S','S'),
  A_SIZE = PACK('S','I','Z','E'),
  A_READ = PACK('R','E','A','D'),
  A_FORK = PACK('F','O','R','K'),
  A_ACCS = PACK('A','C','C','S'),
  A_STAT = PACK('S','T','A','T'),
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
      enum accs_answer flag;
    } accs;
    struct {
      enum accs_answer flag;
      struct stat_answer stat;
    } stat;
  };
} answer_t;

/* "ASK" :P */

enum ask {
  C_TERM = PACK('T','E','R','M'),
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

channel_t *channel_new(int fd);
void channel_free(channel_t *c);

bool channel_handshake(channel_t *c);
bool channel_has_pending_query(channel_t *t, int timeout);
bool channel_read_query(channel_t *t, query_t *r);
void channel_write_ask(channel_t *t, ask_t *a);
void channel_write_answer(channel_t *t, answer_t *a);
void *channel_write_buffer(channel_t *t, size_t n);
void channel_flush(channel_t *t);

void log_query(FILE *f, query_t *q);
#endif /*!SPROTOCOL_H*/
