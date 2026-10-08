#ifndef CAPT_H
#define CAPT_H

#include "usb_comm.h"
#include "status.h"
#include <stdint.h>
#include <stdbool.h>

#define CAPT_IEEE_IDENT          0xA1A0
#define CAPT_GET_PRINTER_INFO    0xA1A1
#define CAPT_RESERVE_UNIT        0xA2A0
#define CAPT_RELEASE_UNIT        0xE0A9
#define CAPT_GET_EXTENDED_STATUS 0xA0A0
#define CAPT_GET_BASIC_STATUS    0xE0A0
#define CAPT_GO_ONLINE           0xE0A5
#define CAPT_GO_OFFLINE          0xE0A6
#define CAPT_BEGIN_PAGE          0xD0A0
#define CAPT_BEGIN_DATA          0xD0A1
#define CAPT_END_PAGE            0xD0A2
#define CAPT_PRINT_DATA          0xC0A0
#define CAPT_CLEAR_ERROR         0xE0A2
#define CAPT_CLEAR_MISPRINT      0xE0A3
#define CAPT_DISCARD_DATA        0xE0A4
#define CAPT_RESET_ENGINE        0xE0A1

#define CAPT_MODEL_LBP810        0x01A4

/* Paper size codes for page params (LBP-810 CAPT v1) */
#define PAPER_A4        0x02
#define PAPER_A5        0x03
#define PAPER_ENV_COM10 0x05
#define PAPER_B5        0x07
#define PAPER_ENV_C5    0x08
#define PAPER_EXEC      0x0A
#define PAPER_ENV_DL    0x0B
#define PAPER_LEGAL     0x0C
#define PAPER_LETTER    0x0D

/* Return codes for capt_print_page / wait_buffer_ready */
#define CAPT_OK                      0
#define CAPT_ERR_COMM               -1
#define CAPT_ERR_NO_PAPER           -2
#define CAPT_ERR_COVER_OPEN         -3
#define CAPT_ERR_CANCELLED          -4
#define CAPT_ERR_DELIVERY_TIMEOUT   -5
#define CAPT_ERR_BUFFER_TIMEOUT     -6
#define CAPT_ERR_ENGINE             -7
#define CAPT_ERR_JAM                -8

typedef struct {
    uint8_t     code;
    const char *name;
    const char *ipp_name;
    uint16_t    width_600;
    uint16_t    height_600;
    uint16_t    margin_l_600;
    uint16_t    margin_t_600;
    uint16_t    line_size_600;
    uint16_t    lines_600;
} capt_paper_info_t;

typedef struct {
    uint16_t target_model;
    uint8_t paper_size;
    uint8_t media_source;
    uint8_t input_slot;
    uint8_t toner_density[4];
    uint8_t mode;
    uint8_t resolution;
    uint8_t constants[4];
    uint8_t smoothing;
    uint8_t toner_saving;
    uint16_t margin_left;
    uint16_t margin_top;
    uint16_t image_line_size;
    uint16_t image_lines;
    uint16_t paper_width;
    uint16_t paper_height;
} capt_page_params_t;

typedef struct {
    usb_device_t usb;
    uint16_t block_size;
    uint16_t page_counter;
    volatile bool *cancel_flag;
} capt_printer_t;

const capt_paper_info_t *capt_get_paper_info_by_code(uint8_t paper_code);
const capt_paper_info_t *capt_get_paper_info_by_name(const char *name);
const capt_paper_info_t *capt_get_paper_info_by_dims(uint32_t width_px, uint32_t height_px, uint32_t dpi);
int capt_build_page_params(capt_page_params_t *params, uint8_t paper_code, uint32_t dpi,
                           uint8_t input_slot, uint8_t toner_saving, uint8_t smoothing);

int capt_open(capt_printer_t *printer);
void capt_close(capt_printer_t *printer);
int capt_job_begin(capt_printer_t *printer);
int capt_job_end(capt_printer_t *printer);
int capt_cancel_job(capt_printer_t *printer);
int capt_print_page(capt_printer_t *printer, const capt_page_params_t *params, const uint8_t *bitmap_1bpp, uint32_t width_bytes, uint32_t height_lines);

#endif

