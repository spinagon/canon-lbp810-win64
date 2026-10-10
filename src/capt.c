#include "capt.h"
#include "scoa.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

static int get_basic_status(capt_printer_t *p, uint8_t *status_byte) {
    if (usb_send_packet(&p->usb, CAPT_GET_BASIC_STATUS, NULL, 0) != 0) return -1;
    uint16_t actual = 0;
    uint8_t buf[1] = {0};
    if (usb_recv_packet(&p->usb, CAPT_GET_BASIC_STATUS, buf, 1, &actual) != 0) return -1;
    if (status_byte) *status_byte = buf[0];
    return 0;
}

static int get_extended_status(capt_printer_t *p, capt_status_t *status) {
    if (usb_send_packet(&p->usb, CAPT_GET_EXTENDED_STATUS, NULL, 0) != 0) return -1;
    uint16_t actual = 0;
    uint8_t buf[256];
    if (usb_recv_packet(&p->usb, CAPT_GET_EXTENDED_STATUS, buf, sizeof(buf), &actual) != 0) return -1;
    if (actual < 16) return -1;
    capt_parse_status(buf, actual, status);
    return 0;
}

static const capt_paper_info_t s_paper_table[] = {
    { PAPER_A4,        "A4",        "iso_a4_210x297mm",          4960, 7014, 112, 119, 592, 6776 },
    { PAPER_LETTER,    "Letter",    "na_letter_8.5x11in",        5100, 6600, 110, 120, 610, 6360 },
    { PAPER_LEGAL,     "Legal",     "na_legal_8.5x14in",         5100, 8400, 110, 120, 610, 8160 },
    { PAPER_EXEC,      "Executive", "na_executive_7.25x10.5in",  4350, 6300, 100, 120, 520, 6060 },
    { PAPER_A5,        "A5",        "iso_a5_148x210mm",          3496, 4960, 100, 110, 412, 4740 },
    { PAPER_B5,        "B5",        "jis_b5_182x257mm",          4299, 6070, 100, 110, 513, 5850 },
    { PAPER_ENV_COM10, "COM10",     "na_number-10_4.125x9.5in",  2475, 5700, 100, 120, 285, 5460 },
    { PAPER_ENV_DL,    "DL",        "iso_dl_110x220mm",          2598, 5196, 100, 110, 300, 4976 },
    { PAPER_ENV_C5,    "C5",        "iso_c5_162x229mm",          3826, 5409, 100, 110, 454, 5189 },
};
#define NUM_PAPERS (sizeof(s_paper_table) / sizeof(s_paper_table[0]))

const capt_paper_info_t *capt_get_paper_info_by_code(uint8_t paper_code) {
    for (size_t i = 0; i < NUM_PAPERS; i++) {
        if (s_paper_table[i].code == paper_code) {
            return &s_paper_table[i];
        }
    }
    return NULL;
}

const capt_paper_info_t *capt_get_paper_info_by_name(const char *name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < NUM_PAPERS; i++) {
        if (strstr(name, s_paper_table[i].ipp_name) != NULL ||
            strstr(name, s_paper_table[i].name) != NULL) {
            return &s_paper_table[i];
        }
    }
    if (strstr(name, "a4") || strstr(name, "A4")) return capt_get_paper_info_by_code(PAPER_A4);
    if (strstr(name, "letter") || strstr(name, "Letter")) return capt_get_paper_info_by_code(PAPER_LETTER);
    if (strstr(name, "legal") || strstr(name, "Legal")) return capt_get_paper_info_by_code(PAPER_LEGAL);
    if (strstr(name, "exec") || strstr(name, "Exec")) return capt_get_paper_info_by_code(PAPER_EXEC);
    if (strstr(name, "a5") || strstr(name, "A5")) return capt_get_paper_info_by_code(PAPER_A5);
    if (strstr(name, "b5") || strstr(name, "B5")) return capt_get_paper_info_by_code(PAPER_B5);
    if (strstr(name, "com10") || strstr(name, "number-10") || strstr(name, "COM10")) return capt_get_paper_info_by_code(PAPER_ENV_COM10);
    if (strstr(name, "dl") || strstr(name, "DL")) return capt_get_paper_info_by_code(PAPER_ENV_DL);
    if (strstr(name, "c5") || strstr(name, "C5")) return capt_get_paper_info_by_code(PAPER_ENV_C5);
    return NULL;
}

const capt_paper_info_t *capt_get_paper_info_by_dims(uint32_t width_px, uint32_t height_px, uint32_t dpi) {
    uint32_t w600 = (dpi > 0 && dpi != 600) ? (width_px * 600 / dpi) : width_px;
    uint32_t h600 = (dpi > 0 && dpi != 600) ? (height_px * 600 / dpi) : height_px;

    const capt_paper_info_t *best = &s_paper_table[0]; // A4 default
    uint32_t min_diff = UINT32_MAX;

    for (size_t i = 0; i < NUM_PAPERS; i++) {
        uint32_t dw = (w600 > s_paper_table[i].width_600) ? (w600 - s_paper_table[i].width_600) : (s_paper_table[i].width_600 - w600);
        uint32_t dh = (h600 > s_paper_table[i].height_600) ? (h600 - s_paper_table[i].height_600) : (s_paper_table[i].height_600 - h600);
        uint32_t diff = dw + dh;
        if (diff < min_diff) {
            min_diff = diff;
            best = &s_paper_table[i];
        }
    }
    return best;
}

int capt_build_page_params(capt_page_params_t *params, uint8_t paper_code, uint32_t dpi,
                           uint8_t input_slot, uint8_t toner_saving, uint8_t smoothing)
{
    const capt_paper_info_t *info = capt_get_paper_info_by_code(paper_code);
    if (!info) {
        info = capt_get_paper_info_by_code(PAPER_A4);
    }
    memset(params, 0, sizeof(*params));
    params->target_model = CAPT_MODEL_LBP810;
    params->paper_size = info->code;
    params->media_source = (input_slot == 0x00) ? 0x00 : 0x01;
    params->input_slot = input_slot;
    memset(params->toner_density, 0x1F, 4);
    params->mode = 0x00;
    params->resolution = (dpi >= 600) ? 0x11 : 0x00;
    params->constants[0] = 0x03;
    params->constants[1] = 0x01;
    params->constants[2] = 0x01;
    params->constants[3] = 0x01;
    params->smoothing = smoothing;
    params->toner_saving = toner_saving;

    if (dpi >= 600) {
        params->paper_width     = info->width_600;
        params->paper_height    = info->height_600;
        params->margin_left     = info->margin_l_600;
        params->margin_top      = info->margin_t_600;
        params->image_line_size = info->line_size_600;
        params->image_lines     = info->lines_600;
    } else {
        params->paper_width     = info->width_600 / 2;
        params->paper_height    = info->height_600 / 2;
        params->margin_left     = info->margin_l_600 / 2;
        params->margin_top      = info->margin_t_600 / 2;
        params->image_line_size = (info->line_size_600 + 1) / 2;
        params->image_lines     = info->lines_600 / 2;
    }
    return 0;
}

static int wait_printer_ready(capt_printer_t *p, int timeout_ms) {
    int elapsed = 0;
    bool warned_paper = false;
    bool warned_cover = false;
    while (elapsed < timeout_ms) {
        if (p->cancel_flag && *(p->cancel_flag)) {
            LOG_WARN("Printer wait aborted: job cancelled");
            return -4;
        }
        capt_status_t status;
        if (get_extended_status(p, &status) == 0) {
            if (status.ready && !status.error && status.paper_available) {
                return 0;
            }
            if (!status.cover_closed && !warned_cover) {
                LOG_WARN("Printer COVER IS OPEN — please close the printer door");
                warned_cover = true;
            }
            if (!status.paper_available && !warned_paper) {
                LOG_WARN("Printer reports OUT OF PAPER / TRAY EMPTY (engine=0x%04X, basic=0x%02X, slots=0x%02X)",
                         status.engine, status.basic, status.paper_slots);
                warned_paper = true;
            }
            if (status.error || status.controller != 0) {
                LOG_WARN("Printer reports error (basic=0x%02X, ctrl=0x%02X, engine=0x%04X), clearing...",
                         status.basic, status.controller, status.engine);
                uint8_t buf[4];
                uint16_t actual = 0;
                usb_send_packet(&p->usb, CAPT_CLEAR_ERROR, NULL, 0);
                usb_recv_packet(&p->usb, CAPT_CLEAR_ERROR, buf, sizeof(buf), &actual);
                usb_send_packet(&p->usb, CAPT_DISCARD_DATA, NULL, 0);
                usb_recv_packet(&p->usb, CAPT_DISCARD_DATA, buf, sizeof(buf), &actual);
                usb_send_packet(&p->usb, CAPT_CLEAR_MISPRINT, NULL, 0);
                usb_recv_packet(&p->usb, CAPT_CLEAR_MISPRINT, buf, sizeof(buf), &actual);
            }
        }
        SLEEP_MS(500);
        elapsed += 500;
    }
    LOG_WARN("Timeout waiting for printer ready");
    return -1;
}

/* Wait until printer buffer has space, with timeout */
static int wait_buffer_ready(capt_printer_t *p, int timeout_ms) {
    int elapsed = 0;
    int last_log = 0;
    while (elapsed < timeout_ms) {
        if (p->cancel_flag && *(p->cancel_flag)) {
            LOG_WARN("Buffer wait aborted: job cancelled");
            return CAPT_ERR_CANCELLED;
        }
        uint8_t sb = 0;
        if (get_basic_status(p, &sb) != 0) return CAPT_ERR_COMM;
        if (sb & 0x80) {
            capt_status_t st;
            memset(&st, 0, sizeof(st));
            get_extended_status(p, &st);
            LOG_ERROR("Printer engine error during raster streaming: %s (engine=0x%04X, basic=0x%02X)",
                      st.error_string, st.engine, sb);
            if (!st.cover_closed) return CAPT_ERR_COVER_OPEN;
            if (st.engine & 0x0100) return CAPT_ERR_JAM;
            if (!st.paper_available) return CAPT_ERR_NO_PAPER;
            return CAPT_ERR_ENGINE;
        }
        if (!(sb & 0x08)) return CAPT_OK; /* Buffer has space */

        SLEEP_MS(20);
        elapsed += 20;
        if (elapsed - last_log >= 3000) {
            LOG_INFO("Printer engine buffer full, waiting for laser scanning to consume raster... (t=%ds, basic=0x%02X)",
                     elapsed / 1000, sb);
            last_log = elapsed;
        }
    }
    uint8_t final_sb = 0;
    get_basic_status(p, &final_sb);
    LOG_ERROR("wait_buffer_ready timed out after %d ms (basic_status=0x%02X)", timeout_ms, final_sb);
    return CAPT_ERR_BUFFER_TIMEOUT;
}

/* Send a chunk of compressed data with flow control */
static int send_data_chunk(capt_printer_t *p, const uint8_t *data, uint32_t len) {
    int rc = wait_buffer_ready(p, 30000); /* 30-second timeout for mechanical engine scanning */
    if (rc != CAPT_OK) return rc;
    return usb_send_packet(&p->usb, CAPT_PRINT_DATA, data, (uint16_t)len);
}

int capt_open(capt_printer_t *printer) {
    memset(printer, 0, sizeof(*printer));
    if (usb_open(&printer->usb) != 0) return -1;
    
    if (usb_send_packet(&printer->usb, CAPT_IEEE_IDENT, NULL, 0) != 0) return -1;
    uint8_t ident[256];
    uint16_t actual = 0;
    if (usb_recv_packet(&printer->usb, CAPT_IEEE_IDENT, ident, sizeof(ident), &actual) != 0) return -1;
    LOG_INFO("Printer ID: %.*s", (int)(actual > 4 ? actual - 4 : 0), ident + 4);
    
    if (usb_send_packet(&printer->usb, CAPT_GET_PRINTER_INFO, NULL, 0) != 0) return -1;
    uint8_t info[64];
    if (usb_recv_packet(&printer->usb, CAPT_GET_PRINTER_INFO, info, sizeof(info), &actual) != 0) return -1;
    
    if (actual >= 4) {
        printer->block_size = info[2] | (info[3] << 8);
    } else {
        printer->block_size = 4096;
    }
    LOG_INFO("Block size: %u bytes", printer->block_size);
    return 0;
}

void capt_close(capt_printer_t *printer) {
    usb_close(&printer->usb);
}

int capt_job_begin(capt_printer_t *printer) {
    LOG_INFO("Beginning CAPT print job...");
    if (usb_send_packet(&printer->usb, CAPT_RESERVE_UNIT, NULL, 0) != 0) return -1;
    uint8_t buf[4];
    uint16_t actual = 0;
    usb_recv_packet(&printer->usb, CAPT_RESERVE_UNIT, buf, sizeof(buf), &actual);
    
    /* Clear any lingering error states */
    usb_send_packet(&printer->usb, CAPT_CLEAR_ERROR, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_CLEAR_ERROR, buf, sizeof(buf), &actual);
    usb_send_packet(&printer->usb, CAPT_DISCARD_DATA, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_DISCARD_DATA, buf, sizeof(buf), &actual);
    usb_send_packet(&printer->usb, CAPT_CLEAR_MISPRINT, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_CLEAR_MISPRINT, buf, sizeof(buf), &actual);

    /* Reset engine state machine if engine was left in an unready/error state */
    capt_status_t st;
    if (get_extended_status(printer, &st) == 0) {
        if (!st.ready || st.error || (st.basic & 0x80) || st.controller != 0) {
            LOG_INFO("Resetting print engine state (engine=0x%04X, ctrl=0x%02X, basic=0x%02X)...",
                     st.engine, st.controller, st.basic);
            usb_send_packet(&printer->usb, CAPT_RESET_ENGINE, NULL, 0);
            usb_recv_packet(&printer->usb, CAPT_RESET_ENGINE, buf, sizeof(buf), &actual);
            SLEEP_MS(500);
        }
    }

    wait_printer_ready(printer, 3000);
    return 0;
}

int capt_job_end(capt_printer_t *printer) {
    if (!printer || !printer->usb.connected) return 0;
    LOG_INFO("Ending CAPT print job...");
    uint8_t buf[4];
    uint16_t actual = 0;

    /* Do NOT send CAPT_GO_OFFLINE (0xE0A6).
     * In CAPT v1, GO_OFFLINE abruptly terminates the engine, fuser, and feed rollers.
     * Captdriver and Nicolas Boichat keep the engine online and only release the unit. */
    if (usb_send_packet(&printer->usb, CAPT_RELEASE_UNIT, NULL, 0) != 0) return -1;
    usb_recv_packet(&printer->usb, CAPT_RELEASE_UNIT, buf, sizeof(buf), &actual);
    return 0;
}

int capt_cancel_job(capt_printer_t *printer) {
    if (!printer || !printer->usb.connected) return 0;
    LOG_WARN("Executing CAPT job cancellation and engine buffer purge (0xE0A4)...");
    uint8_t buf[16];
    uint16_t actual = 0;

    /* 1. Transmit CAPT_DISCARD_DATA (0xE0A4) to flush onboard FIFO buffer */
    usb_send_packet(&printer->usb, CAPT_DISCARD_DATA, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_DISCARD_DATA, buf, sizeof(buf), &actual);

    /* 2. Clear errors and misprints */
    usb_send_packet(&printer->usb, CAPT_CLEAR_ERROR, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_CLEAR_ERROR, buf, sizeof(buf), &actual);
    usb_send_packet(&printer->usb, CAPT_CLEAR_MISPRINT, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_CLEAR_MISPRINT, buf, sizeof(buf), &actual);

    /* 3. Reset print engine */
    usb_send_packet(&printer->usb, CAPT_RESET_ENGINE, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_RESET_ENGINE, buf, sizeof(buf), &actual);

    /* 4. Release unit */
    usb_send_packet(&printer->usb, CAPT_RELEASE_UNIT, NULL, 0);
    usb_recv_packet(&printer->usb, CAPT_RELEASE_UNIT, buf, sizeof(buf), &actual);

    printer->page_counter = 0;
    LOG_INFO("CAPT job cancellation completed");
    return 0;
}

int capt_print_page(capt_printer_t *printer, const capt_page_params_t *params,
                    const uint8_t *bitmap_1bpp, uint32_t width_bytes, uint32_t height_lines) {
    int rc;

    if (printer->cancel_flag && *(printer->cancel_flag)) {
        LOG_WARN("Print page aborted before start: job cancelled");
        return CAPT_ERR_CANCELLED;
    }

    /* Check printer readiness before sending data */
    if (wait_printer_ready(printer, 10000) != 0) {
        if (printer->cancel_flag && *(printer->cancel_flag)) {
            return CAPT_ERR_CANCELLED;
        }
        capt_status_t st;
        get_extended_status(printer, &st);
        if ((st.basic & 0x80) && !st.paper_available) {
            LOG_ERROR("Cannot print page %u: NO PAPER IN PRINTER TRAY (engine=0x%04X, basic=0x%02X, slots=0x%02X)",
                      printer->page_counter + 1, st.engine, st.basic, st.paper_slots);
            LOG_ERROR(">>> PLEASE LOAD PAPER INTO THE PRINTER TRAY AND ENSURE IT IS FULLY INSERTED <<<");
            return CAPT_ERR_NO_PAPER;
        }
        if (!st.cover_closed) {
            LOG_ERROR("Cannot print page %u: PRINTER COVER IS OPEN", printer->page_counter + 1);
            return CAPT_ERR_COVER_OPEN;
        }
        if ((st.basic & 0x80) && (st.engine & 0x0100)) {
            LOG_ERROR("Cannot print page %u: PAPER JAM IN PRINTER", printer->page_counter + 1);
            return CAPT_ERR_JAM;
        }
        return CAPT_ERR_COMM;
    }

    /* Phase 1: Go online */
    uint16_t page_num = printer->page_counter + 1;
    uint8_t magic[8] = {
        0xEE, 0xDB, 0xEA, 0xAD,  /* 0xADEADBEE little-endian */
        (uint8_t)(page_num & 0xFF),
        (uint8_t)((page_num >> 8) & 0xFF),
        0x00, 0x00
    };
    if (usb_send_packet(&printer->usb, CAPT_GO_ONLINE, magic, 8) != 0) return -1;
    uint8_t resp[4] = {0};
    uint16_t actual = 0;
    usb_recv_packet(&printer->usb, CAPT_GO_ONLINE, resp, sizeof(resp), &actual);
    LOG_INFO("CAPT_GO_ONLINE page %u returned 0x%02X", page_num, resp[0]);

    /* Query baseline status AFTER engine is armed and reset for this page */
    capt_status_t init_status;
    memset(&init_status, 0, sizeof(init_status));
    get_extended_status(printer, &init_status);
    uint16_t start_printed = init_status.page_printed;
    uint16_t start_shipped = init_status.page_shipped;
    LOG_INFO("Starting page (job page %u): initial printer printed=%u, shipped=%u, start=%u",
             page_num, start_printed, start_shipped, init_status.page_start);
    
    /* Phase 2: Send page parameters (34-byte payload) */
    uint8_t pdata[34] = {0};
    pdata[0] = 0x00; pdata[1] = 0x00;
    pdata[2] = params->target_model & 0xFF; pdata[3] = (params->target_model >> 8) & 0xFF;
    pdata[4] = params->paper_size; pdata[5] = params->media_source;
    pdata[6] = 0x00; /* LBP-810 physical cassette slot index is always 0x00 */
    pdata[7] = 0x00;
    memcpy(&pdata[8], params->toner_density, 4);
    pdata[12] = params->mode; pdata[13] = params->resolution;
    memcpy(&pdata[14], params->constants, 4);
    pdata[18] = params->smoothing; pdata[19] = params->toner_saving;
    pdata[20] = 0x00; pdata[21] = 0x00;
    pdata[22] = params->margin_left & 0xFF; pdata[23] = (params->margin_left >> 8) & 0xFF;
    pdata[24] = params->margin_top & 0xFF; pdata[25] = (params->margin_top >> 8) & 0xFF;
    pdata[26] = params->image_line_size & 0xFF; pdata[27] = (params->image_line_size >> 8) & 0xFF;
    pdata[28] = params->image_lines & 0xFF; pdata[29] = (params->image_lines >> 8) & 0xFF;
    pdata[30] = params->paper_width & 0xFF; pdata[31] = (params->paper_width >> 8) & 0xFF;
    pdata[32] = params->paper_height & 0xFF; pdata[33] = (params->paper_height >> 8) & 0xFF;
    
    LOG_INFO("CAPT_BEGIN_PAGE: model=0x%04X, paper=0x%02X, %ux%u (linesize=%u, lines=%u)",
             params->target_model, params->paper_size,
             params->paper_width, params->paper_height,
             params->image_line_size, params->image_lines);
    usb_send_packet(&printer->usb, CAPT_BEGIN_PAGE, pdata, 34);

    /* Phase 3: Signal data start */
    usb_send_packet(&printer->usb, CAPT_BEGIN_DATA, NULL, 0);
    
    /* Phase 4: SCoA compress and stream with flow control */
    scoa_ctx_t scoa;
    scoa_init(&scoa, (int)width_bytes);

    for (uint32_t line = 0; line < height_lines; line++) {
        if (printer->cancel_flag && *(printer->cancel_flag)) {
            LOG_WARN("Job cancelled during SCoA streaming at line %u", line);
            scoa_free(&scoa);
            return CAPT_ERR_CANCELLED;
        }

        const uint8_t *cur = bitmap_1bpp + (line * width_bytes);
        const uint8_t *prev = (line > 0)
            ? bitmap_1bpp + ((line - 1) * width_bytes)
            : NULL;
        scoa_compress_line(&scoa, cur, prev);

        /* Flush when buffer reaches block_size */
        while (scoa.out_pos >= printer->block_size) {
            rc = send_data_chunk(printer, scoa.out_buf, printer->block_size);
            if (rc != 0) {
                LOG_ERROR("Failed to send data chunk at line %u: %d", line, rc);
                scoa_free(&scoa);
                return rc;
            }
            /* Shift remaining data in buffer */
            memmove(scoa.out_buf, scoa.out_buf + printer->block_size,
                    scoa.out_pos - printer->block_size);
            scoa.out_pos -= printer->block_size;
        }
    }

    /* Emit EOP and flush remaining */
    scoa_end_page(&scoa);
    if (scoa.out_pos > 0) {
        rc = send_data_chunk(printer, scoa.out_buf, (uint32_t)scoa.out_pos);
        if (rc != 0) {
            LOG_ERROR("Failed to send final data chunk: %d", rc);
            scoa_free(&scoa);
            return rc;
        }
    }
    scoa_free(&scoa);

    /* Phase 5: End page */
    usb_send_packet(&printer->usb, CAPT_END_PAGE, NULL, 0);
    uint8_t post_basic = 0;
    get_basic_status(printer, &post_basic);
    LOG_INFO("Page %u raster sent (basic=0x%02X), waiting for printer engine to deliver...",
             page_num, post_basic);
    
    /* Phase 6: Wait for page delivery (up to 60 seconds) */
    int timeout = 60000;
    int elapsed = 0;
    int last_log_elapsed = 0;
    capt_status_t status;
    bool delivered = false;

    /* Physical laser pickup and heating cycle takes at least 3-4 seconds */
    for (int i = 0; i < 30; i++) {
        if (printer->cancel_flag && *(printer->cancel_flag)) {
            LOG_WARN("Job cancelled while waiting for pickup cycle");
            return CAPT_ERR_CANCELLED;
        }
        SLEEP_MS(100);
        elapsed += 100;
    }

    do {
        if (printer->cancel_flag && *(printer->cancel_flag)) {
            LOG_WARN("Job cancelled while waiting for page delivery");
            return CAPT_ERR_CANCELLED;
        }
        if (get_extended_status(printer, &status) == 0) {
            if (elapsed - last_log_elapsed >= 3000) {
                LOG_INFO("Delivery status (t=%ds): printed=%u (start=%u), shipped=%u (start=%u), printing=%u, engine=0x%04X, basic=0x%02X, aux=0x%02X",
                         elapsed / 1000, status.page_printed, start_printed,
                         status.page_shipped, start_shipped,
                         status.page_printing, status.engine, status.basic, status.aux);
                last_log_elapsed = elapsed;
            }

            if (status.page_printed > start_printed) {
                LOG_INFO("Page %u printed and ejected successfully (counter printed: %u -> %u, shipped: %u -> %u)",
                         page_num, start_printed, status.page_printed,
                         start_shipped, status.page_shipped);
                delivered = true;
                break;
            }
            if (!status.cover_closed) {
                LOG_ERROR("Printer cover opened during page printing");
                break;
            }
            if ((status.basic & 0x80) != 0) {
                LOG_ERROR("Printer error during page printing: %s (engine=0x%04X, basic=0x%02X)",
                          status.error_string, status.engine, status.basic);
                break;
            }
            /* Engine completion fallback: if at least 10 seconds have elapsed and the engine is idle, ready, and feed rollers stopped */
            if (elapsed >= 10000 && status.ready && ((status.basic & 0x80) == 0) && (status.aux & 0x06) == 0) {
                LOG_INFO("Printer engine cycle completed (engine=0x%04X, basic=0x%02X, aux=0x%02X)",
                         status.engine, status.basic, status.aux);
                delivered = true;
                break;
            }
        }
        SLEEP_MS(1000);
        elapsed += 1000;
    } while (elapsed < timeout);

    if (!delivered && elapsed >= timeout) {
        LOG_WARN("Timeout waiting for page delivery");
    }

    printer->page_counter++;
    return delivered ? CAPT_OK : CAPT_ERR_DELIVERY_TIMEOUT;
}

