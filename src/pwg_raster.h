#ifndef PWG_RASTER_H
#define PWG_RASTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    /**** Standard Page Device Dictionary String Values (256 bytes) ****/
    char     media_class[64];              /* offset 0 */
    char     media_color[64];              /* offset 64 */
    char     media_type[64];               /* offset 128 */
    char     output_type[64];              /* offset 192 */

    /**** Standard Page Device Dictionary Integer Values (116 bytes) ****/
    uint32_t advance_distance;             /* offset 256 */
    uint32_t advance_media;                /* offset 260 */
    uint32_t collate;                      /* offset 264 */
    uint32_t cut_media;                    /* offset 268 */
    uint32_t duplex;                       /* offset 272 */
    uint32_t hw_resolution_x;              /* offset 276 */
    uint32_t hw_resolution_y;              /* offset 280 */
    uint32_t imaging_bounding_box[4];      /* offset 284..296 */
    uint32_t insert_sheet;                 /* offset 300 */
    uint32_t jog;                          /* offset 304 */
    uint32_t leading_edge;                 /* offset 308 */
    uint32_t margins[2];                   /* offset 312..316 */
    uint32_t manual_feed;                  /* offset 320 */
    uint32_t media_position;               /* offset 324 */
    uint32_t media_weight;                 /* offset 328 */
    uint32_t mirror_print;                 /* offset 332 */
    uint32_t negative_print;               /* offset 336 */
    uint32_t num_copies;                   /* offset 340 */
    uint32_t orientation;                  /* offset 344 */
    uint32_t output_face_up;               /* offset 348 */
    uint32_t page_size_x;                  /* offset 352 */
    uint32_t page_size_y;                  /* offset 356 */
    uint32_t separations;                  /* offset 360 */
    uint32_t tray_switch;                  /* offset 364 */
    uint32_t tumble;                       /* offset 368 */

    /**** Page Device Dictionary Values (48 bytes) ****/
    uint32_t width;                        /* offset 372 */
    uint32_t height;                       /* offset 376 */
    uint32_t media_type_code;              /* offset 380 */
    uint32_t bits_per_color;               /* offset 384 */
    uint32_t bits_per_pixel;               /* offset 388 */
    uint32_t bytes_per_line;               /* offset 392 */
    uint32_t color_order;                  /* offset 396 */
    uint32_t color_space;                  /* offset 400 */
    uint32_t compression;                  /* offset 404 */
    uint32_t row_count;                    /* offset 408 */
    uint32_t row_feed;                     /* offset 412 */
    uint32_t row_step;                     /* offset 416 */

    /**** Version 2 Dictionary Values ****/
    uint32_t num_colors;                   /* offset 420 */
    float    borderless_scaling_factor;    /* offset 424 */
    float    page_size_f[2];               /* offset 428..432 */
    float    imaging_bbox_f[4];            /* offset 436..448 */
    uint32_t pwg_integer[16];              /* offset 452..512 */
    float    pwg_real[16];                 /* offset 516..576 */
    char     pwg_string[16][64];           /* offset 580..1604 */
    char     marker_type[64];              /* offset 1604..1668 */
    char     rendering_intent[64];         /* offset 1668..1732 */
    char     page_size_name[64];           /* offset 1732..1796 */
} pwg_page_header_t;

_Static_assert(sizeof(pwg_page_header_t) == 1796, "pwg_page_header_t must be exactly 1796 bytes");

typedef struct {
    const uint8_t *data;
    size_t data_len;
    size_t offset;
} pwg_stream_t;

typedef void (*pwg_line_callback_t)(const uint8_t *line_data, int line_bytes,
                                     int line_number, void *user_data);

int pwg_open(pwg_stream_t *stream, const uint8_t *data, size_t len);
int pwg_read_page_header(pwg_stream_t *stream, pwg_page_header_t *hdr);
int pwg_read_line(pwg_stream_t *stream, const pwg_page_header_t *hdr,
                  uint8_t *line_out, uint32_t *line_repeat);
int pwg_decode_page(pwg_stream_t *stream, const pwg_page_header_t *hdr,
                    pwg_line_callback_t callback, void *user_data);
bool pwg_has_more_pages(const pwg_stream_t *stream);

#endif // PWG_RASTER_H
