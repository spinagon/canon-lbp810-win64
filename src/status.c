#include "status.h"
#include <string.h>

void capt_parse_status(const uint8_t *raw, uint16_t raw_len, capt_status_t *status) {
    if (!status || raw_len < 16) return;
    
    status->basic = raw[0];
    status->aux = raw[2];
    status->controller = raw[3];
    status->paper_slots = raw[4];
    
    // Flags based on basic byte
    status->ready = (status->basic & 0x02) == 0; // 0x02: NOT_READY
    status->buffer_full = (status->basic & 0x08) != 0; // 0x08: IM_DATA_BUSY
    
    // Standard CAPT v1 extended status positions
    status->engine = raw[6] | (raw[7] << 8);
    status->page_start = raw[8] | (raw[9] << 8);
    status->page_printing = raw[10] | (raw[11] << 8);
    status->page_shipped = raw[12] | (raw[13] << 8);
    status->page_printed = raw[14] | (raw[15] << 8);
    
    // EngineReadyStatus bitmasks: 0x0200 = NO_PRINT_PAPER, 0x2000 = NO_CARTRIDGE, 0x4000 = DOOR_OPEN
    status->paper_available = (status->engine & 0x0200) == 0;
    status->cartridge_present = (status->engine & 0x2000) == 0;
    status->cover_closed = (status->engine & 0x4000) == 0;

    // Error flag: active if fatal comm error or any hardware fault
    status->error = ((status->basic & 0x80) != 0) ||
                    !status->cover_closed ||
                    !status->cartridge_present ||
                    ((status->engine & 0x0100) != 0) || /* Jam */
                    ((status->engine & 0x0002) != 0);   /* Service Call */
    
    status->error_string = capt_status_error_string(status);
}

const char *capt_status_error_string(const capt_status_t *status) {
    if (!status->cover_closed) return "Cover Open";
    if (!status->cartridge_present) return "No Toner Cartridge";
    if (!status->paper_available) return "Out of Paper / No Paper in Tray";
    if (status->engine & 0x0100) return "Paper Jam";
    if (status->engine & 0x00C0) return "Misprint Error";
    if (status->engine & 0x0002) return "Service Call (Hardware Error)";
    if (status->basic & 0x80) return "General Error";
    if (!status->ready) return "Printer Not Ready";
    return "No Error";
}

bool capt_status_is_ready(const capt_status_t *status) {
    return status->ready && !status->error && status->paper_available;
}

bool capt_status_can_send_data(const capt_status_t *status) {
    return !status->buffer_full && capt_status_is_ready(status);
}
