#ifndef TEXPRESSO_PROTOCOL_H_
#define TEXPRESSO_PROTOCOL_H_

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

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
char *txp_open(txp_client *client, txp_file_id file, const char *path, bool for_write);

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

#endif // TEXPRESSO_PROTOCOL_H_
