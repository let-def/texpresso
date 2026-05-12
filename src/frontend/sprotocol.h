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

typedef struct channel_s IOChannel;
typedef int file_id;

#define LOG 0

#define LEN(txt) (sizeof(txt)-1)
#define STR(X) #X
#define SSTR(X) STR(X)

#define pabort() \
  do { perror(__FILE__ ":" SSTR(__LINE__)); myabort(); } while(0)

#define mabort(...) \
  do { fprintf(stderr, "Aborting from " __FILE__ ":" SSTR(__LINE__) "\n" __VA_ARGS__); print_backtrace(); abort(); } while(0)

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)

/* QUERIES */

enum ProtocolQuery {
  Q_OPRD = PACK('O','P','R','D'),
  Q_OPWR = PACK('O','P','W','R'),
  Q_READ = PACK('R','E','A','D'),
  Q_APND = PACK('A','P','N','D'),
  Q_CLOS = PACK('C','L','O','S'),
  Q_SIZE = PACK('S','I','Z','E'),
  Q_MTIM = PACK('M','T','I','M'),
  Q_SEEN = PACK('S','E','E','N'),
  Q_GPIC = PACK('G','P','I','C'),
  Q_SPIC = PACK('S','P','I','C'),
  Q_CHLD = PACK('C','H','L','D'),
};

enum TxpFileKind
{
  TXP_KIND_AFM           = PACK('A','F','M', 0 ),
  TXP_KIND_BIB           = PACK('B','I','B', 0 ),
  TXP_KIND_BST           = PACK('B','S','T', 0 ),
  TXP_KIND_CMAP          = PACK('C','M','A','P'),
  TXP_KIND_CNF           = PACK('C','N','F', 0 ),
  TXP_KIND_ENC           = PACK('E','N','C', 0 ),
  TXP_KIND_FORMAT        = PACK('F','R','M','T'),
  TXP_KIND_FONT_MAP      = PACK('F','M','A','P'),
  TXP_KIND_MISC_FONTS    = PACK('M','F','N','T'),
  TXP_KIND_OFM           = PACK('O','F','M', 0 ),
  TXP_KIND_OPEN_TYPE     = PACK('O','T','F', 0 ),
  TXP_KIND_OVF           = PACK('O','V','F', 0 ),
  TXP_KIND_PICT          = PACK('P','I','C','T'),
  TXP_KIND_PK            = PACK('P','K', 0 , 0 ),
  TXP_KIND_PROGRAM_DATA  = PACK('P','D','A','T'),
  TXP_KIND_SFD           = PACK('S','F','D', 0 ),
  TXP_KIND_PRIMARY       = PACK('P','R','I','M'),
  TXP_KIND_TEX           = PACK('T','E','X', 0 ),
  TXP_KIND_TEX_PS_HEADER = PACK('T','P','S','H'),
  TXP_KIND_TFM           = PACK('T','F','M', 0 ),
  TXP_KIND_TRUE_TYPE     = PACK('T','T','F', 0 ),
  TXP_KIND_TYPE1         = PACK('T','Y','P','1'),
  TXP_KIND_VF            = PACK('V','F', 0 , 0 ),
  TXP_KIND_OTHER         = PACK('O','T','H','R'),
};

struct pic_cache {
  int type, page;
  float bounds[4];
};

typedef struct {
  int time;
  enum ProtocolQuery tag;
  union {
    struct {
      file_id fid;
      char *path;
      enum TxpFileKind kind;
    } open;
    struct {
      file_id fid;
      int pos, size;
    } read;
    struct {
      file_id fid;
      int size;
      char *buf;
    } apnd;
    struct {
      file_id fid;
    } clos;
    struct {
      file_id fid;
    } size;
    struct {
      file_id fid;
    } mtim;
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
} ProtocolQuery;

/* ANSWERS */

enum ProtocolAnswer {
  A_DONE = PACK('D','O','N','E'),
  A_PASS = PACK('P','A','S','S'),
  A_SIZE = PACK('S','I','Z','E'),
  A_MTIM = PACK('M','T','I','M'),
  A_READ = PACK('R','E','A','D'),
  A_FORK = PACK('F','O','R','K'),
  A_OPEN = PACK('O','P','E','N'),
  A_GPIC = PACK('G','P','I','C'),
};

#define READ_FORK (-1)

typedef struct {
  enum ProtocolAnswer tag;
  union {
    struct {
      uint32_t size;
    } size;
    struct {
      uint32_t mtime;
    } mtim;
    struct {
      int size;
    } read;
    struct {
      int path_len;
    } open;
    struct {
      float bounds[4];
    } gpic;
  };
} ProtocolAnswer;

/* "ASK" :P */

enum ProtocolAsk {
  C_FLSH = PACK('F','L','S','H'),
};

typedef struct {
  enum ProtocolAsk tag;
  union {
    struct {
      int pid;
    } term;
    struct {
      int fid, pos;
    } Fence;
    struct {
      int fid;
    } flsh;
  };
} ProtocolAsk;

/* Functions */

IOChannel *channel_new(void);
void channel_free(IOChannel *c);

bool channel_handshake(IOChannel *c, int fd);
bool channel_has_pending_query(IOChannel *t, int fd, int timeout);
enum ProtocolQuery channel_peek_query(IOChannel *t, int fd);
bool channel_read_query(IOChannel *t, int fd, ProtocolQuery *r);
void channel_write_ask(IOChannel *t, int fd, ProtocolAsk *a);
void channel_write_answer(IOChannel *t, int fd, ProtocolAnswer *a);
void *channel_get_buffer(IOChannel *t, size_t n);
void channel_flush(IOChannel *t, int fd);
void channel_reset(IOChannel *t);

void log_query(FILE *f, ProtocolQuery *q);
#endif /*!SPROTOCOL_H*/
