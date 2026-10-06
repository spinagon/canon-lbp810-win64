#include "dither.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t s_bayer8x8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

void dither_init(dither_ctx_t *ctx, int width) {
    ctx->width = width;
    ctx->error_cur   = (int16_t *)calloc(width + 4, sizeof(int16_t));
    ctx->error_next  = (int16_t *)calloc(width + 4, sizeof(int16_t));
    ctx->error_next2 = (int16_t *)calloc(width + 4, sizeof(int16_t));
}

void dither_free(dither_ctx_t *ctx) {
    if (ctx->error_cur) {
        free(ctx->error_cur);
        ctx->error_cur = NULL;
    }
    if (ctx->error_next) {
        free(ctx->error_next);
        ctx->error_next = NULL;
    }
    if (ctx->error_next2) {
        free(ctx->error_next2);
        ctx->error_next2 = NULL;
    }
    ctx->width = 0;
}

void dither_line(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out) {
    int width = ctx->width;
    memset(mono_out, 0, (width + 7) / 8);
    
    for (int x = 0; x < width; x++) {
        int16_t ink = 255 - gray_in[x];
        int16_t val = ink + ctx->error_cur[x + 1];
        int16_t err = 0;
        
        if (val >= 128) {
            err = val - 255;
            mono_out[x / 8] |= (1 << (7 - (x % 8)));
        } else {
            err = val;
        }
        
        // Floyd-Steinberg error diffusion
        ctx->error_cur[x + 2] += (err * 7) / 16;
        ctx->error_next[x]     += (err * 3) / 16;
        ctx->error_next[x + 1] += (err * 5) / 16;
        ctx->error_next[x + 2] += (err * 1) / 16;
    }
    
    int16_t *temp = ctx->error_cur;
    ctx->error_cur = ctx->error_next;
    ctx->error_next = temp;
    memset(ctx->error_next, 0, (width + 4) * sizeof(int16_t));
}

void dither_line_adaptive(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out) {
    int width = ctx->width;
    memset(mono_out, 0, (width + 7) / 8);

    for (int x = 0; x < width; x++) {
        uint8_t g = gray_in[x];
        /* Extreme values: direct quantization with ZERO error diffusion to keep text crisp */
        if (g >= 245) {
            continue; // Pure/near-white: no dot, no diffusion
        } else if (g <= 10) {
            mono_out[x / 8] |= (1 << (7 - (x % 8))); // Pure/near-black: solid dot, no diffusion
            continue;
        }

        /* Continuous-tone midtones: Floyd-Steinberg diffusion */
        int16_t ink = 255 - g;
        int16_t val = ink + ctx->error_cur[x + 1];
        int16_t err = 0;

        if (val >= 128) {
            err = val - 255;
            mono_out[x / 8] |= (1 << (7 - (x % 8)));
        } else {
            err = val;
        }

        ctx->error_cur[x + 2] += (err * 7) / 16;
        ctx->error_next[x]     += (err * 3) / 16;
        ctx->error_next[x + 1] += (err * 5) / 16;
        ctx->error_next[x + 2] += (err * 1) / 16;
    }

    int16_t *temp = ctx->error_cur;
    ctx->error_cur = ctx->error_next;
    ctx->error_next = temp;
    memset(ctx->error_next, 0, (width + 4) * sizeof(int16_t));
}

void dither_line_atkinson(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out) {
    int width = ctx->width;
    memset(mono_out, 0, (width + 7) / 8);

    for (int x = 0; x < width; x++) {
        int16_t ink = 255 - gray_in[x];
        int16_t val = ink + ctx->error_cur[x + 1];
        int16_t err = 0;

        if (val >= 128) {
            err = val - 255;
            mono_out[x / 8] |= (1 << (7 - (x % 8)));
        } else {
            err = val;
        }

        int16_t e8 = err / 8;
        ctx->error_cur[x + 2]   += e8;
        ctx->error_cur[x + 3]   += e8;
        if (x > 0) {
            ctx->error_next[x]   += e8;
        }
        ctx->error_next[x + 1]  += e8;
        ctx->error_next[x + 2]  += e8;
        ctx->error_next2[x + 1] += e8;
    }

    /* Rotate 3 buffers */
    int16_t *old_cur = ctx->error_cur;
    ctx->error_cur = ctx->error_next;
    ctx->error_next = ctx->error_next2;
    ctx->error_next2 = old_cur;
    memset(ctx->error_next2, 0, (width + 4) * sizeof(int16_t));
}

void dither_line_bayer8x8(const uint8_t *gray_in, uint8_t *mono_out, int width, int y) {
    memset(mono_out, 0, (width + 7) / 8);
    const uint8_t *matrix_row = s_bayer8x8[y % 8];
    for (int x = 0; x < width; x++) {
        int ink = 255 - gray_in[x];
        int threshold = (matrix_row[x % 8] * 255) / 63;
        if (ink > threshold) {
            mono_out[x / 8] |= (1 << (7 - (x % 8)));
        }
    }
}

void threshold_line(const uint8_t *gray_in, uint8_t *mono_out, int width, uint8_t threshold) {
    memset(mono_out, 0, (width + 7) / 8);
    for (int x = 0; x < width; x++) {
        int ink = 255 - gray_in[x];
        if (ink >= threshold) {
            mono_out[x / 8] |= (1 << (7 - (x % 8)));
        }
    }
}

void dither_render_line(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out, int y, dither_algorithm_t algo) {
    switch (algo) {
    case DITHER_ADAPTIVE:
        dither_line_adaptive(ctx, gray_in, mono_out);
        break;
    case DITHER_ATKINSON:
        dither_line_atkinson(ctx, gray_in, mono_out);
        break;
    case DITHER_BAYER_8X8:
        dither_line_bayer8x8(gray_in, mono_out, ctx->width, y);
        break;
    case DITHER_THRESHOLD:
        threshold_line(gray_in, mono_out, ctx->width, 128);
        break;
    case DITHER_FLOYD_STEINBERG:
    default:
        dither_line(ctx, gray_in, mono_out);
        break;
    }
}
