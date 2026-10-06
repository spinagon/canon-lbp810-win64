#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/pwg_raster.h"
#include "../src/dither.h"

// Buffer for mock data
static uint8_t mock_pwg[8192];
static size_t mock_pwg_len = 0;

static void write_be32(uint8_t *p, uint32_t val) {
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8) & 0xFF;
    p[3] = val & 0xFF;
}

static void create_mock_pwg() {
    mock_pwg_len = 0;
    // Magic
    memcpy(&mock_pwg[mock_pwg_len], "RaS2", 4);
    mock_pwg_len += 4;
    
    // Header
    uint8_t *hdr = &mock_pwg[mock_pwg_len];
    memset(hdr, 0, 1796);
    mock_pwg_len += 1796;
    
    write_be32(hdr + 372, 16); // width
    write_be32(hdr + 376, 4);  // height
    write_be32(hdr + 384, 8);  // bits_per_color
    write_be32(hdr + 388, 8);  // bits_per_pixel
    write_be32(hdr + 392, 16); // bytes_per_line
    write_be32(hdr + 400, 3);  // color_space (sGray)
    write_be32(hdr + 420, 1);  // num_colors
    
    // Raster Data - PWG 5102.4 Section 4.4
    // Line 1: rep=1 (0), non-repeating run of 16 (257 - 16 = 241)
    mock_pwg[mock_pwg_len++] = 0;        // Repetition count - 1 = 0 (1 repeat)
    mock_pwg[mock_pwg_len++] = 257 - 16; // 16 non-repeating bytes
    for (int i = 0; i < 16; i++) mock_pwg[mock_pwg_len++] = 255; // White
    
    // Line 2: rep=1 (0), repeating run of 16 (16 - 1 = 15)
    mock_pwg[mock_pwg_len++] = 0;        // Repetition count - 1 = 0
    mock_pwg[mock_pwg_len++] = 15;       // Repeat next pixel 16 times
    mock_pwg[mock_pwg_len++] = 0;        // Black
    
    // Line 3: rep=1 (0), non-repeating 8 + repeating 8
    mock_pwg[mock_pwg_len++] = 0;        // Repetition count - 1 = 0
    mock_pwg[mock_pwg_len++] = 257 - 8;  // 8 non-repeating bytes
    for (int i = 0; i < 8; i++) mock_pwg[mock_pwg_len++] = 128; // Gray
    mock_pwg[mock_pwg_len++] = 7;        // Repeat next pixel 8 times
    mock_pwg[mock_pwg_len++] = 64;       // Dark Gray
    
    // Line 4: rep=1 (0), non-repeating run of 16 gradient
    mock_pwg[mock_pwg_len++] = 0;        // Repetition count - 1 = 0
    mock_pwg[mock_pwg_len++] = 257 - 16; // 16 non-repeating bytes
    for (int i = 0; i < 16; i++) mock_pwg[mock_pwg_len++] = (i * 16); // Gradient
}

static int lines_decoded = 0;

static void line_cb(const uint8_t *line_data, int line_bytes, int line_number, void *user_data) {
    (void)user_data;
    assert(line_bytes == 16);
    assert(line_number == lines_decoded);
    
    if (line_number == 0) {
        for (int i=0; i<16; i++) assert(line_data[i] == 255);
    } else if (line_number == 1) {
        for (int i=0; i<16; i++) assert(line_data[i] == 0);
    } else if (line_number == 2) {
        for (int i=0; i<8; i++) assert(line_data[i] == 128);
        for (int i=8; i<16; i++) assert(line_data[i] == 64);
    } else if (line_number == 3) {
        for (int i=0; i<16; i++) assert(line_data[i] == (i * 16));
    }
    
    lines_decoded++;
}

static void test_pwg() {
    create_mock_pwg();
    pwg_stream_t stream;
    assert(pwg_open(&stream, mock_pwg, mock_pwg_len) == 0);
    assert(pwg_has_more_pages(&stream));
    
    pwg_page_header_t hdr;
    assert(pwg_read_page_header(&stream, &hdr) == 0);
    assert(hdr.width == 16);
    assert(hdr.height == 4);
    assert(hdr.bits_per_pixel == 8);
    assert(hdr.bytes_per_line == 16);
    
    lines_decoded = 0;
    assert(pwg_decode_page(&stream, &hdr, line_cb, NULL) == 0);
    assert(lines_decoded == 4);
    
    assert(!pwg_has_more_pages(&stream));
    printf("PWG Raster tests passed.\\n");
}

static void test_dither() {
    int width = 16;
    dither_ctx_t ctx;
    dither_init(&ctx, width);
    
    uint8_t in_white[16];
    memset(in_white, 255, 16);
    uint8_t out[2];
    
    // Floyd-Steinberg
    dither_line(&ctx, in_white, out);
    assert(out[0] == 0 && out[1] == 0); // No dots for white
    
    uint8_t in_black[16];
    memset(in_black, 0, 16);
    dither_line(&ctx, in_black, out);
    assert(out[0] == 0xFF && out[1] == 0xFF); // All dots for black
    
    // Threshold
    uint8_t in_gray[16];
    memset(in_gray, 128, 16);
    threshold_line(in_gray, out, 16, 127);
    assert(out[0] == 0xFF && out[1] == 0xFF); 
    
    threshold_line(in_gray, out, 16, 128);
    assert(out[0] == 0x00 && out[1] == 0x00);

    // Adaptive halftoning (Roadmap 4.1)
    // Pure white with ink=0 must NOT accumulate or diffuse error
    dither_line_adaptive(&ctx, in_white, out);
    assert(out[0] == 0 && out[1] == 0);
    for (int i = 0; i < width + 4; i++) {
        assert(ctx.error_cur[i] == 0);
    }

    // Pure black must produce all 1s without residual error
    dither_line_adaptive(&ctx, in_black, out);
    assert(out[0] == 0xFF && out[1] == 0xFF);
    for (int i = 0; i < width + 4; i++) {
        assert(ctx.error_cur[i] == 0);
    }

    // Atkinson Dithering (Roadmap 4.2)
    dither_line_atkinson(&ctx, in_white, out);
    assert(out[0] == 0 && out[1] == 0);
    dither_line_atkinson(&ctx, in_black, out);
    assert(out[0] == 0xFF && out[1] == 0xFF);

    // Atkinson with mid-tone gray (128) produces checker/diffusion pattern
    dither_line_atkinson(&ctx, in_gray, out);
    assert(out[0] != 0 && out[0] != 0xFF); // Mixed dots

    // Bayer 8x8 Ordered Dithering (Roadmap 4.2)
    dither_line_bayer8x8(in_white, out, width, 0);
    assert(out[0] == 0 && out[1] == 0);
    dither_line_bayer8x8(in_black, out, width, 0);
    assert(out[0] == 0xFF && out[1] == 0xFF);
    dither_line_bayer8x8(in_gray, out, width, 0);
    assert(out[0] != 0 && out[0] != 0xFF); // Deterministic thresholded dots

    // Dither render dispatch for all supported algorithms
    dither_render_line(&ctx, in_white, out, 0, DITHER_FLOYD_STEINBERG);
    assert(out[0] == 0 && out[1] == 0);
    dither_render_line(&ctx, in_white, out, 0, DITHER_ADAPTIVE);
    assert(out[0] == 0 && out[1] == 0);
    dither_render_line(&ctx, in_black, out, 0, DITHER_ATKINSON);
    assert(out[0] == 0xFF && out[1] == 0xFF);
    dither_render_line(&ctx, in_black, out, 0, DITHER_BAYER_8X8);
    assert(out[0] == 0xFF && out[1] == 0xFF);
    dither_render_line(&ctx, in_gray, out, 0, DITHER_THRESHOLD);
    assert(out[0] == 0x00 && out[1] == 0x00);
    
    dither_free(&ctx);
    printf("Dither multi-algorithm tests passed.\n");
}

int main() {
    test_pwg();
    test_dither();
    printf("All test_pwg_dither tests passed!\n");
    return 0;
}
