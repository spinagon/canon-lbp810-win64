#ifndef SCOA_H
#define SCOA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *out_buf;
    size_t   out_pos;
    size_t   out_capacity;
    int      line_bytes;
} scoa_ctx_t;

void   scoa_init(scoa_ctx_t *ctx, int line_bytes);
size_t scoa_compress_line(scoa_ctx_t *ctx, const uint8_t *line, const uint8_t *prev_line);
size_t scoa_end_page(scoa_ctx_t *ctx);
void   scoa_reset(scoa_ctx_t *ctx);
void   scoa_free(scoa_ctx_t *ctx);

int scoa_decompress(const uint8_t *compressed, size_t comp_len,
                    uint8_t *output, int line_bytes, int num_lines);

#endif // SCOA_H
