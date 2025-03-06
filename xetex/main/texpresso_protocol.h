#ifndef TEXPRESSO_PROTOCOL_H_
#define TEXPRESSO_PROTOCOL_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#define FOURCC(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)

// File formats recognized
enum txp_file_kind
{
  TXP_KIND_AFM           = FOURCC('A','F','M', 0 ),
  TXP_KIND_BIB           = FOURCC('B','I','B', 0 ),
  TXP_KIND_BST           = FOURCC('B','S','T', 0 ),
  TXP_KIND_CMAP          = FOURCC('C','M','A','P'),
  TXP_KIND_CNF           = FOURCC('C','N','F', 0 ),
  TXP_KIND_ENC           = FOURCC('E','N','C', 0 ),
  TXP_KIND_FORMAT        = FOURCC('F','R','M','T'),
  TXP_KIND_FONT_MAP      = FOURCC('F','M','A','P'),
  TXP_KIND_MISC_FONTS    = FOURCC('M','F','N','T'),
  TXP_KIND_OFM           = FOURCC('O','F','M', 0 ),
  TXP_KIND_OPEN_TYPE     = FOURCC('O','T','F', 0 ),
  TXP_KIND_OVF           = FOURCC('O','V','F', 0 ),
  TXP_KIND_PICT          = FOURCC('P','I','C','T'),
  TXP_KIND_PK            = FOURCC('P','K', 0 , 0 ),
  TXP_KIND_PROGRAM_DATA  = FOURCC('P','D','A','T'),
  TXP_KIND_SFD           = FOURCC('S','F','D', 0 ),
  TXP_KIND_PRIMARY       = FOURCC('P','R','I','M'),
  TXP_KIND_TEX           = FOURCC('T','E','X', 0 ),
  TXP_KIND_TEX_PS_HEADER = FOURCC('T','P','S','H'),
  TXP_KIND_TFM           = FOURCC('T','F','M', 0 ),
  TXP_KIND_TRUE_TYPE     = FOURCC('T','T','F', 0 ),
  TXP_KIND_TYPE1         = FOURCC('T','Y','P','1'),
  TXP_KIND_VF            = FOURCC('V','F', 0 , 0 ),
  TXP_KIND_OTHER         = FOURCC('O','T','H','R'),
};

// Open for read or for write
enum txp_open_mode
{
  TXP_READ,
  TXP_WRITE
};

extern pid_t texpresso_fork_with_channel(int fd, uint32_t time);

// File descriptors and client identifiers
typedef int32_t txp_file_id;
typedef int32_t txp_client_id;

// txp_client structure (forward declaration)
typedef struct txp_client txp_client;

// Connect to a client
txp_client *txp_connect(FILE *f);

// Get the current generation of the client
uint32_t txp_generation(txp_client *client);

// Bump the generation of the client
void txp_bump_generation(txp_client *client);

// Flush all operations
void txp_flush(txp_client *client);

// Open a file
char *txp_open(txp_client *client, txp_file_id file, const char *path, enum txp_file_kind kind, enum txp_open_mode mode);

// Read from a file
size_t txp_read(txp_client *client, txp_file_id file, uint32_t pos, void *buf, size_t len);

// Write to a file
void txp_write(txp_client *client, txp_file_id file, uint32_t pos, const void *buf, size_t len);

// Close a file
void txp_close(txp_client *client, txp_file_id file);

// Mark a position as seen
void txp_seen(txp_client *client, txp_file_id file, uint32_t pos);

// Get cached bounds of graphic object (gpic)
bool txp_gpic(txp_client *client, const char *path, int typ, int page, float *bounds);

// Cache bounds of graphic object (spic)
void txp_spic(txp_client *client, const char *path, int typ, int page, const float *bounds);

// Fork the client
pid_t txp_fork(txp_client *client);

// File mtime
uint32_t txp_mtime(txp_client *client, txp_file_id file);

// File size
uint32_t txp_size(txp_client *client, txp_file_id file);

#endif // TEXPRESSO_PROTOCOL_H_
