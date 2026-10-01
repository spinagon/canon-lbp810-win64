#ifndef DITHER_H
#define DITHER_H

#include <stdint.h>

typedef struct {
    int      width;           // Width in pixels
    int16_t *error_cur;       // Current error row (width + 2 elements)
    int16_t *error_next;      // Next error row (width + 2 elements)
} dither_ctx_t;

void dither_init(dither_ctx_t *ctx, int width);
void dither_free(dither_ctx_t *ctx);
void dither_line(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out);
void threshold_line(const uint8_t *gray_in, uint8_t *mono_out, int width, uint8_t threshold);

#endif // DITHER_H
