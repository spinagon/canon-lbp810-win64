#include "scoa.h"
#include <stdlib.h>
#include <string.h>

void scoa_init(scoa_ctx_t *ctx, int line_bytes) {
    ctx->line_bytes = line_bytes;
    ctx->out_capacity = line_bytes * 2 + 8192;
    ctx->out_buf = malloc(ctx->out_capacity);
    ctx->out_pos = 0;
}

void scoa_reset(scoa_ctx_t *ctx) {
    ctx->out_pos = 0;
}

void scoa_free(scoa_ctx_t *ctx) {
    if (ctx->out_buf) {
        free(ctx->out_buf);
        ctx->out_buf = NULL;
    }
    ctx->out_capacity = 0;
    ctx->out_pos = 0;
}

static void ensure_capacity(scoa_ctx_t *ctx, size_t needed) {
    if (ctx->out_pos + needed > ctx->out_capacity) {
        ctx->out_capacity = (ctx->out_capacity + needed) * 2;
        ctx->out_buf = realloc(ctx->out_buf, ctx->out_capacity);
    }
}

static void emit_u8(scoa_ctx_t *ctx, uint8_t v) {
    ensure_capacity(ctx, 1);
    ctx->out_buf[ctx->out_pos++] = v;
}

static inline int get_match_len(const uint8_t *p1, const uint8_t *p2, int max_len) {
    if (!p2) return 0;
    int i = 0;
    while (i < max_len && p1[i] == p2[i]) i++;
    return i;
}

static inline int get_repeat_len(const uint8_t *p, int max_len) {
    if (max_len == 0) return 0;
    int i = 1;
    uint8_t v = p[0];
    while (i < max_len && p[i] == v) i++;
    return i;
}

size_t scoa_compress_line(scoa_ctx_t *ctx, const uint8_t *line, const uint8_t *prev_line) {
    size_t start_pos = ctx->out_pos;
    int x = 0;
    
    while (x < ctx->line_bytes) {
        int C = get_match_len(line + x, prev_line ? prev_line + x : NULL, ctx->line_bytes - x);
        
        if (x + C == ctx->line_bytes) {
            emit_u8(ctx, 0x41); // EOL
            break;
        }
        
        if (C >= 8) {
            int chunks = C / 8;
            if (chunks > 31) chunks = 31;
            emit_u8(ctx, 0x80 | chunks);
            x += chunks * 8;
            continue;
        }
        
        int nx = x + C;
        int R = get_repeat_len(line + nx, ctx->line_bytes - nx);
        uint8_t val = line[nx];
        
        if (R >= 2) {
            if (R > 255) R = 255;
            
            if (val == 0x43 && C == 0) {
                // RepeatX
                if (R <= 7) {
                    emit_u8(ctx, 0xC0 | (R << 3));
                } else {
                    emit_u8(ctx, 0xA0 | (R >> 3));
                    emit_u8(ctx, 0x00 | ((R & 7) << 3));
                }
                x += R;
                continue;
            }
            
            // RepeatThenRaw
            if (C == 0 && R >= 2 && R <= 7 && val != 0x43) {
                int L = 0;
                int rx = nx + R;
                while (L < 255 && rx + L < ctx->line_bytes) {
                    if (get_match_len(line + rx + L, prev_line ? prev_line + rx + L : NULL, ctx->line_bytes - (rx + L)) >= 2) break;
                    if (get_repeat_len(line + rx + L, ctx->line_bytes - (rx + L)) >= 2) break;
                    L++;
                }
                
                if (L > 0) {
                    if (L <= 7) {
                        emit_u8(ctx, 0xC0 | (R << 3) | L);
                    } else {
                        emit_u8(ctx, 0xA0 | (L >> 3));
                        emit_u8(ctx, 0x40 | (R << 3) | (L & 7));
                    }
                    emit_u8(ctx, val);
                    for (int i = 0; i < L; i++) emit_u8(ctx, line[rx + i]);
                    x += R + L;
                    continue;
                }
            }
            
            // CopyThenRepeat
            if (R <= 7) {
                emit_u8(ctx, 0x40 | (R << 3) | C);
                emit_u8(ctx, val);
            } else {
                emit_u8(ctx, 0xA0 | (R >> 3));
                emit_u8(ctx, 0x80 | ((R & 7) << 3) | C);
                emit_u8(ctx, val);
            }
            x += C + R;
            continue;
        }
        
        // Raw sequence
        int L = 1;
        while (L < 255 && nx + L < ctx->line_bytes) {
            if (get_match_len(line + nx + L, prev_line ? prev_line + nx + L : NULL, ctx->line_bytes - (nx + L)) >= 2) break;
            if (get_repeat_len(line + nx + L, ctx->line_bytes - (nx + L)) >= 2) break;
            L++;
        }
        
        if (L <= 7) {
            emit_u8(ctx, 0x00 | (L << 3) | C);
        } else {
            emit_u8(ctx, 0xA0 | (L >> 3));
            emit_u8(ctx, 0xC0 | ((L & 7) << 3) | C);
        }
        for (int i = 0; i < L; i++) {
            emit_u8(ctx, line[nx + i]);
        }
        x += C + L;
    }
    
    return ctx->out_pos - start_pos;
}

size_t scoa_end_page(scoa_ctx_t *ctx) {
    size_t start_pos = ctx->out_pos;
    if (ctx->out_pos % 2 != 0) {
        emit_u8(ctx, 0x40); // NOP
    }
    emit_u8(ctx, 0x42); // EOP
    return ctx->out_pos - start_pos;
}

int scoa_decompress(const uint8_t *compressed, size_t comp_len,
                    uint8_t *output, int line_bytes, int num_lines) {
    size_t in_pos = 0;
    int y = 0;
    int x = 0;
    
    while (in_pos < comp_len && y < num_lines) {
        uint8_t cmd = compressed[in_pos++];
        if (cmd == 0x40) continue;
        
        if (cmd == 0x41) {
            if (y > 0) {
                for (; x < line_bytes; x++) output[y * line_bytes + x] = output[(y - 1) * line_bytes + x];
            } else {
                for (; x < line_bytes; x++) output[y * line_bytes + x] = 0;
            }
            y++;
            x = 0;
            continue;
        }
        
        if (cmd == 0x42) break;
        
        int ext = 0;
        if ((cmd & 0xE0) == 0xA0) {
            ext = cmd & 0x1F;
            if (in_pos >= comp_len) return -1;
            cmd = compressed[in_pos++];
        }
        
        int C = 0, R = 0, L = 0;
        uint8_t val = 0;
        int is_raw = 0, is_repeat = 0;
        
        if (ext == 0) {
            if ((cmd & 0xE0) == 0xC0) {
                R = (cmd >> 3) & 7;
                if (R == 0) {
                    C = cmd & 7;
                } else {
                    L = cmd & 7;
                    if (L == 0) {
                        is_repeat = 1; val = 0x43;
                    } else {
                        is_repeat = 1; is_raw = 1;
                        if (in_pos >= comp_len) return -1;
                        val = compressed[in_pos++];
                    }
                }
            } else if ((cmd & 0xE0) == 0x80) {
                C = (cmd & 0x1F) * 8;
            } else if ((cmd & 0xC0) == 0x40) {
                R = (cmd >> 3) & 7;
                C = cmd & 7;
                is_repeat = 1;
                if (in_pos >= comp_len) return -1;
                val = compressed[in_pos++];
            } else if ((cmd & 0xC0) == 0x00) {
                L = (cmd >> 3) & 7;
                C = cmd & 7;
                is_raw = 1;
            }
        } else {
            if ((cmd & 0xC0) == 0x80) {
                R = (ext << 3) | ((cmd >> 3) & 7);
                C = cmd & 7;
                is_repeat = 1;
                if (in_pos >= comp_len) return -1;
                val = compressed[in_pos++];
            } else if ((cmd & 0xC0) == 0xC0) {
                L = (ext << 3) | ((cmd >> 3) & 7);
                C = cmd & 7;
                is_raw = 1;
            } else if ((cmd & 0xC0) == 0x40) {
                L = (ext << 3) | (cmd & 7);
                R = (cmd >> 3) & 7;
                is_repeat = 1;
                is_raw = 1;
                if (in_pos >= comp_len) return -1;
                val = compressed[in_pos++];
            } else if ((cmd & 0xC0) == 0x00) {
                R = (ext << 3) | ((cmd >> 3) & 7);
                is_repeat = 1;
                val = 0x43;
            } else {
                return -1;
            }
        }
        
        for (int i = 0; i < C; i++) {
            if (x >= line_bytes) { x = 0; y++; }
            if (y >= num_lines) return -1;
            output[y * line_bytes + x] = (y > 0) ? output[(y - 1) * line_bytes + x] : 0;
            x++;
        }
        
        if (is_repeat) {
            for (int i = 0; i < R; i++) {
                if (x >= line_bytes) { x = 0; y++; }
                if (y >= num_lines) return -1;
                output[y * line_bytes + x] = val;
                x++;
            }
        }
        
        if (is_raw) {
            for (int i = 0; i < L; i++) {
                if (in_pos >= comp_len) return -1;
                if (x >= line_bytes) { x = 0; y++; }
                if (y >= num_lines) return -1;
                output[y * line_bytes + x] = compressed[in_pos++];
                x++;
            }
        }
        
        if (x == line_bytes) {
            x = 0;
            y++;
        }
    }
    
    return in_pos;
}
