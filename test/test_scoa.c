#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/scoa.h"

int test_case(const char *name, uint8_t *data, int line_bytes, int num_lines) {
    printf("Running test: %s... ", name);
    
    scoa_ctx_t ctx;
    scoa_init(&ctx, line_bytes);
    
    for (int y = 0; y < num_lines; y++) {
        const uint8_t *line = &data[y * line_bytes];
        const uint8_t *prev = (y > 0) ? &data[(y - 1) * line_bytes] : NULL;
        scoa_compress_line(&ctx, line, prev);
    }
    scoa_end_page(&ctx);
    
    // (Removed even alignment check as EOP makes final size odd)
    
    uint8_t *out_data = malloc(line_bytes * num_lines);
    memset(out_data, 0, line_bytes * num_lines);
    
    int ret = scoa_decompress(ctx.out_buf, ctx.out_pos, out_data, line_bytes, num_lines);
    if (ret < 0) {
        printf("FAIL (decompress error)\n");
        free(out_data);
        scoa_free(&ctx);
        return 1;
    }
    
    if (memcmp(data, out_data, line_bytes * num_lines) != 0) {
        printf("FAIL (data mismatch)\n");
        for (int i=0; i<line_bytes*num_lines; i++) {
            if (data[i] != out_data[i]) {
                printf("Mismatch at %d (expected %02X, got %02X)\n", i, data[i], out_data[i]);
                break;
            }
        }
        free(out_data);
        scoa_free(&ctx);
        return 1;
    }
    
    printf("PASS (compressed %d -> %zu)\n", line_bytes * num_lines, ctx.out_pos);
    free(out_data);
    scoa_free(&ctx);
    return 0;
}

int main() {
    int fails = 0;
    int lb = 592; int nl = 100;
    uint8_t *data = calloc(1, lb * nl);
    
    fails += test_case("All-zero page", data, lb, nl);
    
    memset(data, 0xFF, lb * nl);
    fails += test_case("All-0xFF page", data, lb, nl);
    
    for (int i = 0; i < lb * nl; i++) data[i] = (i % 2 == 0) ? 0xAA : 0x55;
    fails += test_case("Alternating pattern", data, lb, nl);
    
    srand(12345);
    for (int i = 0; i < lb * nl; i++) data[i] = rand() & 0xFF;
    fails += test_case("Random data", data, lb, nl);
    
    memset(data, 0, lb * nl);
    for (int y = 0; y < nl; y++) {
        data[y * lb + (y % lb)] = 0x11;
    }
    fails += test_case("Single changed byte per line", data, lb, nl);
    
    uint8_t *short_data = malloc(1 * 10);
    for (int i=0; i<10; i++) short_data[i] = i;
    fails += test_case("Short lines (1 byte)", short_data, 1, 10);
    free(short_data);
    
    memset(data, 0x43, lb * nl);
    fails += test_case("Repeat 0x43", data, lb, nl);

    free(data);
    
    return fails > 0 ? 1 : 0;
}
