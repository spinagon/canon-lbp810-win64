#ifndef DITHER_H
#define DITHER_H

#include <stdint.h>

typedef enum {
    DITHER_FLOYD_STEINBERG = 0,
    DITHER_ADAPTIVE        = 1,
    DITHER_ATKINSON        = 2,
    DITHER_BAYER_8X8       = 3,
    DITHER_THRESHOLD       = 4
} dither_algorithm_t;

typedef struct {
    int      width;           // Width in pixels
    int16_t *error_cur;       // Current error row (width + 4 elements)
    int16_t *error_next;      // Next error row (width + 4 elements)
    int16_t *error_next2;     // Next+1 error row for Atkinson (width + 4 elements)
} dither_ctx_t;

void dither_init(dither_ctx_t *ctx, int width);
void dither_free(dither_ctx_t *ctx);
void dither_line(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out);
void dither_line_adaptive(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out);
void dither_line_atkinson(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out);
void dither_line_bayer8x8(const uint8_t *gray_in, uint8_t *mono_out, int width, int y);
void threshold_line(const uint8_t *gray_in, uint8_t *mono_out, int width, uint8_t threshold);
void dither_render_line(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out, int y, dither_algorithm_t algo);

#endif // DITHER_H
