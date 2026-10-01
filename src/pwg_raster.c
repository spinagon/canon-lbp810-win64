#include "pwg_raster.h"
#include <string.h>
#include <stdlib.h>

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int pwg_open(pwg_stream_t *stream, const uint8_t *data, size_t len) {
    if (len < 4) return -1;
    if (data[0] != 'R' || data[1] != 'a' || data[2] != 'S' || data[3] != '2') return -1;
    stream->data = data;
    stream->data_len = len;
    stream->offset = 4;
    return 0;
}

bool pwg_has_more_pages(const pwg_stream_t *stream) {
    return stream->offset + 1796 <= stream->data_len;
}

int pwg_read_page_header(pwg_stream_t *stream, pwg_page_header_t *hdr) {
    if (stream->offset + 1796 > stream->data_len) return -1;
    
    const uint8_t *p = stream->data + stream->offset;
    memset(hdr, 0, sizeof(*hdr));

    /* First 4 strings: 256 bytes */
    memcpy(hdr->media_class, p, 64);
    memcpy(hdr->media_color, p + 64, 64);
    memcpy(hdr->media_type, p + 128, 64);
    memcpy(hdr->output_type, p + 192, 64);
    
    /* 42 integer fields: offset 256 to 424 */
    uint32_t *u32_ptr = &hdr->advance_distance;
    for (int i = 0; i < 42; i++) {
        u32_ptr[i] = read_be32(p + 256 + i * 4);
    }

    /* User integer values (offset 452) */
    for (int i = 0; i < 16; i++) {
        hdr->pwg_integer[i] = read_be32(p + 452 + i * 4);
    }

    /* Trailing string names */
    memcpy(hdr->marker_type, p + 1604, 64);
    memcpy(hdr->rendering_intent, p + 1668, 64);
    memcpy(hdr->page_size_name, p + 1732, 64);
    
    stream->offset += 1796;
    return 0;
}

int pwg_read_line(pwg_stream_t *stream, const pwg_page_header_t *hdr,
                  uint8_t *line_out, uint32_t *line_repeat) {
    if (!stream || !hdr || !line_out || !line_repeat) return -1;
    if (stream->offset >= stream->data_len) return -1;

    /* PWG 5102.4 Section 4.4:
     * Each line begins with a repetition count from 1 to 256
     * encoded as a single octet containing (count - 1). */
    uint8_t rep_byte = stream->data[stream->offset++];
    *line_repeat = (uint32_t)rep_byte + 1;

    uint32_t bytes_per_line = hdr->bytes_per_line;
    uint32_t chunk_size = (hdr->bits_per_pixel >= 8) ? (hdr->bits_per_pixel / 8) : 1;
    if (chunk_size == 0) chunk_size = 1;

    memset(line_out, 0xFF, bytes_per_line); /* default to white */
    uint32_t bytes_decoded = 0;

    while (bytes_decoded < bytes_per_line && stream->offset < stream->data_len) {
        uint8_t c = stream->data[stream->offset++];

        if (c < 128) {
            /* 1 to 128 repeated colors: count = c + 1 */
            uint32_t count = (uint32_t)c + 1;
            uint32_t run_bytes = count * chunk_size;

            if (stream->offset + chunk_size > stream->data_len)
                return -1;

            const uint8_t *pixel = stream->data + stream->offset;
            stream->offset += chunk_size;

            uint32_t to_copy = run_bytes;
            if (bytes_decoded + to_copy > bytes_per_line)
                to_copy = bytes_per_line - bytes_decoded;

            for (uint32_t i = 0; i < to_copy; i += chunk_size) {
                uint32_t n = (to_copy - i < chunk_size) ? (to_copy - i) : chunk_size;
                memcpy(line_out + bytes_decoded + i, pixel, n);
            }
            bytes_decoded += to_copy;
        } else if (c > 128) {
            /* 2 to 128 non-repeating colors: count = 257 - c */
            uint32_t count = 257 - (uint32_t)c;
            uint32_t run_bytes = count * chunk_size;

            if (stream->offset + run_bytes > stream->data_len)
                return -1;

            uint32_t to_copy = run_bytes;
            if (bytes_decoded + to_copy > bytes_per_line)
                to_copy = bytes_per_line - bytes_decoded;

            memcpy(line_out + bytes_decoded, stream->data + stream->offset, to_copy);
            stream->offset += run_bytes;
            bytes_decoded += to_copy;
        } else {
            /* c == 128: clear remainder of line to white */
            break;
        }
    }

    if (bytes_decoded < bytes_per_line) {
        memset(line_out + bytes_decoded, 0xFF, bytes_per_line - bytes_decoded);
    }

    return 0;
}

int pwg_decode_page(pwg_stream_t *stream, const pwg_page_header_t *hdr,
                    pwg_line_callback_t callback, void *user_data) {
    if (!stream || !hdr || !callback) return -1;

    uint8_t *line_buffer = (uint8_t *)malloc(hdr->bytes_per_line);
    if (!line_buffer) return -1;

    uint32_t line_number = 0;
    while (line_number < hdr->height && stream->offset < stream->data_len) {
        uint32_t line_repeat = 0;
        if (pwg_read_line(stream, hdr, line_buffer, &line_repeat) != 0) {
            free(line_buffer);
            return -1;
        }

        for (uint32_t r = 0; r < line_repeat && line_number < hdr->height; r++) {
            callback(line_buffer, (int)hdr->bytes_per_line, (int)line_number, user_data);
            line_number++;
        }
    }

    free(line_buffer);
    return (line_number >= hdr->height) ? 0 : -1;
}

