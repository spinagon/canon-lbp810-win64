#ifndef STATUS_H
#define STATUS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  basic;
    uint8_t  aux;
    uint8_t  controller;
    uint8_t  paper_slots;
    uint16_t engine;
    uint16_t page_start;
    uint16_t page_printing;
    uint16_t page_shipped;
    uint16_t page_printed;
    bool     ready;
    bool     paper_available;
    bool     cartridge_present;
    bool     cover_closed;
    bool     buffer_full;
    bool     error;
    const char *error_string;
} capt_status_t;

void capt_parse_status(const uint8_t *raw, uint16_t raw_len, capt_status_t *status);
const char *capt_status_error_string(const capt_status_t *status);
bool capt_status_is_ready(const capt_status_t *status);
bool capt_status_can_send_data(const capt_status_t *status);

#endif
