#include "dither.h"
#include <stdlib.h>
#include <string.h>

void dither_init(dither_ctx_t *ctx, int width) {
    ctx->width = width;
    ctx->error_cur = (int16_t *)calloc(width + 2, sizeof(int16_t));
    ctx->error_next = (int16_t *)calloc(width + 2, sizeof(int16_t));
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
    ctx->width = 0;
}

void dither_line(dither_ctx_t *ctx, const uint8_t *gray_in, uint8_t *mono_out) {
    int width = ctx->width;
    memset(mono_out, 0, (width + 7) / 8);
    
    for (int x = 0; x < width; x++) {
        // sGray: 0 = black, 255 = white. Convert to ink density: 0 = white, 255 = black
        int16_t ink = 255 - gray_in[x];
        int16_t val = ink + ctx->error_cur[x + 1]; // Offset by 1 for left padding
        
        int16_t err = 0;
        
        if (val >= 128) {
            err = val - 255;
            mono_out[x / 8] |= (1 << (7 - (x % 8)));
        } else {
            err = val;
        }
        
        // Floyd-Steinberg error diffusion
        //   X   7
        // 3 5 1
        ctx->error_cur[x + 2] += (err * 7) / 16;
        ctx->error_next[x]     += (err * 3) / 16;
        ctx->error_next[x + 1] += (err * 5) / 16;
        ctx->error_next[x + 2] += (err * 1) / 16;
    }
    
    // Swap buffers and clear next
    int16_t *temp = ctx->error_cur;
    ctx->error_cur = ctx->error_next;
    ctx->error_next = temp;
    memset(ctx->error_next, 0, (width + 2) * sizeof(int16_t));
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
