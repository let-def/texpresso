#include "texpresso_protocol.h"
#include <string.h>

#define BUF_SIZE 4096

struct txp_client
{
  FILE *file;
  // start_time : ProcessTime;
  // delta : Duration;
  uint32_t generation;
  uint32_t seen_pos;
  txp_file_id seen_file;
  txp_file_id append_file;
  int append_len;
  char append_buf[BUF_SIZE];
};

/* QUERIES */

enum tag
{
  T_CLOS = FOURCC('C', 'L', 'O', 'S'),
  T_DONE = FOURCC('D', 'O', 'N', 'E'),
  T_FLSH = FOURCC('F', 'L', 'S', 'H'),
  T_FORK = FOURCC('F', 'O', 'R', 'K'),
  T_GPIC = FOURCC('G', 'P', 'I', 'C'),
  T_OPRD = FOURCC('O', 'P', 'R', 'D'),
  T_OPWR = FOURCC('O', 'P', 'W', 'R'),
  T_OPEN = FOURCC('O', 'P', 'E', 'N'),
  T_PASS = FOURCC('P', 'A', 'S', 'S'),
  T_READ = FOURCC('R', 'E', 'A', 'D'),
  T_SEEN = FOURCC('S', 'E', 'E', 'N'),
  T_SIZE = FOURCC('S', 'I', 'Z', 'E'),
  T_SPIC = FOURCC('S', 'P', 'I', 'C'),
  T_APND = FOURCC('A', 'P', 'N', 'D'),
  T_MTIM = FOURCC('M', 'T', 'I', 'M'),
};

_Noreturn
static void ppanic(const char *txt)
{
    perror(txt);
    exit(1);
}

static void write_or_panic(FILE *file, const void *data, int len)
{
  if (fwrite(data, 1, len, file) != len)
    ppanic("Cannot write to server");
}

static void read_exact(FILE *file, void *data, int len)
{
  if (fread(data, 1, len, file) != len)
    ppanic("Cannot read from server");
}

txp_client *txp_connect(FILE *file)
{
  write_or_panic(file, "TEXPRESSOC01", 12);
  if (fflush(file) != 0)
    ppanic("Cannot flush");

  char buf[12];
  read_exact(file, buf, 12);
  if (memcmp (buf, "TEXPRESSOS01", 12) != 0)
    ppanic("Invalid handshake");
  fprintf(stderr, "texpresso: handshake success\n");

  txp_client *client = calloc(sizeof(txp_client), 1);
  if (client == NULL)
    ppanic("Cannot allocate client");

  client->file = file;
  client->generation = 0;
  client->seen_file = -1;
  client->seen_pos = 0;
  client->append_file = -1;
  client->append_len = 0;

  // FIXME
  // start_time : ProcessTime::now(), delta : Duration::ZERO, generation : 0,
  return client;
}

static void txp_io_send_str(txp_client *io, const char *str)
{
  write_or_panic(io->file, str, strlen(str) + 1);
}

static void txp_io_send4(txp_client *io, const uint8_t data[4])
{
  write_or_panic(io->file, data, 4);
}

#define DECOMPOSE4(i) \
  (i) & 0xFF, ((i) >> 8) & 0xFF, ((i) >> 16) & 0xFF, ((i) >> 24) & 0xFF

static void txp_io_send_u32(txp_client *io, uint32_t i)
{
  uint8_t buf[4] = {DECOMPOSE4(i)};
  write_or_panic(io->file, buf, 4);
}

static uint32_t txp_io_recv_u32(txp_client *io)
{
  uint8_t buf[4];
  read_exact(io->file, buf, 4);
  return (buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24));
}

static void txp_io_send_f32(txp_client *io, float f)
{
  union {
    float f;
    uint32_t u;
  } x;
  x.f = f;
  txp_io_send_u32(io, x.u);
}

static float txp_io_recv_f32(txp_client *io)
{
  union {
    float f;
    uint32_t u;
  } x;
  x.u = txp_io_recv_u32(io);
  return x.f;
}

static void txp_io_send_tag_raw(txp_client *io, enum tag t)
{
  txp_io_send_u32(io, t);
  uint32_t time = 0; // FIXME
  //let time = self.delta + ProcessTime::elapsed(&self.start_time);
  txp_io_send_u32(io, time);
}

static void txp_io_flush(txp_client *io)
{
  if (fflush(io->file) != 0)
    ppanic("Cannot flush");
}

static enum tag txp_io_recv_tag(txp_client *io)
{
  while (1)
  {
    enum tag t = txp_io_recv_u32(io);
    if (t != T_FLSH)
      return t;
    io->generation += 1;
  }
}

_Noreturn
static void panic_tag(enum tag t)
{
  fprintf(stderr, "TeXpresso: unexpected tag %c%c%c%c", DECOMPOSE4(t));
  exit(1);
}

static void txp_io_check_done(txp_client *io) {
  enum tag t = txp_io_recv_tag(io);
  if (t != T_DONE)
    panic_tag(t);
}

static void txp_io_seen(txp_client *io, txp_file_id file, uint32_t pos)
{
  txp_io_send_tag_raw(io, T_SEEN);
  txp_io_send_u32(io, file);
  txp_io_send_u32(io, pos);
}

static void txp_flush_pending(txp_client *io)
{
  if (io->seen_pos != 0)
  {
    txp_io_seen(io, io->seen_file, io->seen_pos);
    io->seen_pos = 0;
    io->seen_file = -1;
  }
  if (io->append_len != 0)
  {
    txp_io_send_tag_raw(io, T_APND);
    txp_io_send_u32(io, io->append_file);
    txp_io_send_u32(io, io->append_len);
    write_or_panic(io->file, io->append_buf, io->append_len);
    txp_io_check_done(io);
    io->append_file = -1;
    io->append_len = 0;
  }
}

void txp_seen(txp_client *io, txp_file_id file, uint32_t pos)
{
  if (io->seen_file != file)
  {
    txp_flush_pending(io);
    io->seen_file = file;
  };
  if (io->seen_pos < pos)
    io->seen_pos = pos;
}

static void txp_io_send_tag(txp_client *io, enum tag t)
{
  txp_flush_pending(io);
  txp_io_send_tag_raw(io, t);
}

char *txp_open(txp_client *io,
               txp_file_id file,
               const char *path,
               enum txp_file_kind kind,
               enum txp_open_mode mode)
{
  txp_io_send_tag(io, (mode == TXP_READ) ? T_OPRD : T_OPWR);
  txp_io_send_u32(io, file);
  txp_io_send_str(io, path);
  txp_io_send_u32(io, kind);
  enum tag t = txp_io_recv_tag(io);
  switch (t)
  {
    case T_PASS:
      return NULL;
    case T_OPEN:
    {
      uint32_t size = txp_io_recv_u32(io);
      char *buf = calloc(1, size + 1);
      if (buf == NULL)
      {
        fprintf(stderr, "Cannot allocate filename (length: %d)\n", size);
        exit(1);
      }
      read_exact(io->file, buf, size);
      buf[size] = 0;
      return buf;
    }
    default:
      panic_tag(t);
  }
}

size_t txp_read(txp_client *io,
                txp_file_id file,
                uint32_t pos,
                void *buf,
                size_t len)
{
  txp_io_send_tag(io, T_READ);
  txp_io_send_u32(io, file);
  txp_io_send_u32(io, pos);
  txp_io_send_u32(io, len);
  enum tag t = txp_io_recv_tag(io);
  switch (t)
  {
    case T_FORK:
      return -1;
    case T_READ:
    {
      uint32_t size = txp_io_recv_u32(io);
      if (size > len)
        exit(1);
      read_exact(io->file, buf, size);
      return size;
    }
    default:
      panic_tag(t);
  }
}

void txp_append(txp_client *io, txp_file_id file, const void *buf, size_t len)
{
  if (len == 0)
    return;

  if (io->append_file == file &&
      io->append_len + len <= sizeof(io->append_buf))
  {
    memcpy(io->append_buf + io->append_len, buf, len);
    io->append_len += len;
    return;
  }

  txp_flush_pending(io);
  if (len <= sizeof(io->append_buf))
  {
    io->append_file = file;
    io->append_len = len;
    memcpy(io->append_buf, buf, len);
    return;
  }

  txp_io_send_tag(io, T_APND);
  txp_io_send_u32(io, file);
  txp_io_send_u32(io, len);
  write_or_panic(io->file, buf, len);
  txp_io_check_done(io);
}

void txp_putc(txp_client *io, txp_file_id file, int c)
{
  if (io->append_file == file && io->append_len < sizeof(io->append_buf))
  {
    io->append_buf[io->append_len++] = c;
    return;
  }
  txp_flush_pending(io);
  io->append_buf[0] = c;
  io->append_len = 1;
}

void txp_close(txp_client *io, txp_file_id file)
{
  txp_io_send_tag(io, T_CLOS);
  txp_io_send_u32(io, file);
  txp_io_check_done(io);
}

pid_t txp_fork(txp_client *io)
{
  io->generation += 1;
  txp_flush(io);

  // FIXME
  uint32_t delta = 0;
  // let delta = self.delta + ProcessTime::elapsed(&self.start_time);
  // let result = texpresso_fork_with_channel(self.file.as_raw_fd(),
  //                                          delta.as_millis() as u32);
  int fd = fileno(io->file);
  if (fd == -1)
    ppanic("fork_with_channel: fileno");
  pid_t result = texpresso_fork_with_channel(fd, delta);
  if (result == 0)
  {
    // FIXME
    // self.delta = delta;
    // self.start_time = ProcessTime::now();
  };
  return result;
}

bool txp_gpic(txp_client *io,
              const char *path,
              int32_t typ,
              int32_t page,
              float bounds[4])
{
  txp_io_send_tag(io, T_GPIC);
  txp_io_send_str(io, path);
  txp_io_send_u32(io, typ);
  txp_io_send_u32(io, page);
  enum tag t = txp_io_recv_tag(io);
  switch (t)
  {
    case T_PASS:
      return 0;
    case T_GPIC:
    {
      bounds[0] = txp_io_recv_f32(io);
      bounds[1] = txp_io_recv_f32(io);
      bounds[2] = txp_io_recv_f32(io);
      bounds[3] = txp_io_recv_f32(io);
      return 1;
    }
    default:
      panic_tag(t);
  }
}

void txp_spic(txp_client *io,
              const char *path,
              int32_t typ,
              int32_t page,
              const float bounds[4])
{
  txp_io_send_tag(io, T_SPIC);
  txp_io_send_str(io, path);
  txp_io_send_u32(io, typ);
  txp_io_send_u32(io, page);
  txp_io_send_f32(io, bounds[0]);
  txp_io_send_f32(io, bounds[1]);
  txp_io_send_f32(io, bounds[2]);
  txp_io_send_f32(io, bounds[3]);
  txp_io_check_done(io);
}

// Get the current generation of the client
uint32_t txp_generation(txp_client *client)
{
  return client->generation;
}

// Bump the generation of the client
void txp_bump_generation(txp_client *client)
{
  client->generation += 1;
}

void txp_flush(txp_client *io)
{
  txp_flush_pending(io);
  txp_io_flush(io);
}

// File mtime
uint32_t txp_mtime(txp_client *io, txp_file_id file)
{
  txp_io_send_tag(io, T_MTIM);
  txp_io_send_u32(io, file);
  enum tag t = txp_io_recv_tag(io);
  if (t != T_MTIM)
    panic_tag(t);
  return txp_io_recv_u32(io);
}

// File size
uint32_t txp_size(txp_client *io, txp_file_id file)
{
  txp_io_send_tag(io, T_SIZE);
  txp_io_send_u32(io, file);
  enum tag t = txp_io_recv_tag(io);
  if (t != T_SIZE)
    panic_tag(t);
  return txp_io_recv_u32(io);
}
