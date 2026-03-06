#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <zlib.h>
#include "tectonic_bridge_core.h"
#include "tectonic_bridge_flate.h"
#include "tectonic_bridge_core_generated.h"

FlateResult tectonic_flate_compress(uint8_t *output_ptr,
                                    uint64_t *output_len,
                                    const uint8_t *input_ptr,
                                    uint64_t input_len,
                                    uint32_t compression_level)
{
  abort();
}

FlateResult tectonic_flate_decompress(uint8_t *output_ptr,
                                      unsigned long *output_len,
                                      const uint8_t *input_ptr,
                                      uint64_t input_len)
{
  return uncompress(output_ptr, output_len, input_ptr, input_len);
}

void *tectonic_flate_new_decompressor(const uint8_t *input_ptr,
                                      uint64_t input_len)
{
  z_stream *strm = calloc(sizeof(z_stream), 1);
  int ret = inflateInit(strm);
  if (ret != Z_OK)
  {
    free(strm);
    return NULL;
  }
  strm->avail_in = input_len;
  strm->next_in = (void*)input_ptr;
  return strm;
}

int tectonic_flate_decompress_chunk(void *handle,
                                    uint8_t *output_ptr,
                                    uint64_t *output_len)
{
  z_stream *strm = handle;
  strm->avail_out = *output_len;
  strm->next_out = output_ptr;
  int ret = inflate(strm, Z_NO_FLUSH);
  *output_len -= strm->avail_out;
  return (ret != Z_OK && ret != Z_STREAM_END);
}

void tectonic_flate_free_decompressor(void *handle)
{
  z_stream *strm = handle;
  inflateEnd(strm);
  free(strm);
}

#include "picohash.h"

int ttbc_get_data_md5(const uint8_t *data,
                      size_t len,
                      uint8_t *digest)
{
  _picohash_md5_ctx_t ctx;
  _picohash_md5_init(&ctx);
  _picohash_md5_update(&ctx, data, len);
  _picohash_md5_final(&ctx, digest);
  return 1;
}

int ttstub_get_file_md5(const char *path,
                        char *digest)
{
  ttbc_input_handle_t *input = ttstub_input_open(path, TTBC_FILE_FORMAT_PROGRAM_DATA, 0);

  if (!input)
    return 0;
  _picohash_md5_ctx_t ctx;
  _picohash_md5_init(&ctx);

  char buf [4096];
  size_t r;

  while ((r = ttstub_input_read(input, buf, 4096)) > 0)
    _picohash_md5_update(&ctx, buf, r);

  ttstub_input_close(input);

  _picohash_md5_final(&ctx, digest);

  return 1;
}
