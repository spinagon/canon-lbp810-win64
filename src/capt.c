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
    capt_parse_status(buf, actual, status);
    if (actual >= 16) {
        LOG_DEBUG("Extended status [%uB]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X (state: %s)",
                  actual, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                  buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
                  status->error_string);
    }
    return 0;
}

static int wait_printer_ready(capt_printer_t *p, int timeout_ms) {
    int elapsed = 0;
    bool warned_paper = false;
    bool warned_cover = false;
    while (elapsed < timeout_ms) {
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
    while (elapsed < timeout_ms) {
        uint8_t sb = 0;
        if (get_basic_status(p, &sb) != 0) return -1;
        if (sb & 0x80) {
            LOG_ERROR("Basic status reports ERROR bit: 0x%02X", sb);
            return -2;  /* Error */
        }
        if (!(sb & 0x08)) return 0; /* Buffer not full */
        SLEEP_MS(10);
        elapsed += 10;
    }
    uint8_t final_sb = 0;
    get_basic_status(p, &final_sb);
    LOG_ERROR("wait_buffer_ready timed out after %d ms (basic_status=0x%02X)", timeout_ms, final_sb);
    return -3; /* Timeout */
}

/* Send a chunk of compressed data with flow control */
static int send_data_chunk(capt_printer_t *p, const uint8_t *data, uint32_t len) {
    int rc = wait_buffer_ready(p, 5000);
    if (rc != 0) return rc;
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
        if (!st.ready || st.engine != 0 || st.controller != 0) {
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

int capt_print_page(capt_printer_t *printer, const capt_page_params_t *params,
                    const uint8_t *bitmap_1bpp, uint32_t width_bytes, uint32_t height_lines) {
    int rc;

    /* Check printer readiness before sending data */
    if (wait_printer_ready(printer, 5000) != 0) {
        capt_status_t st;
        get_extended_status(printer, &st);
        if (!st.paper_available) {
            LOG_ERROR("Cannot print page %u: NO PAPER IN PRINTER TRAY (engine=0x%04X, basic=0x%02X, slots=0x%02X)",
                      printer->page_counter + 1, st.engine, st.basic, st.paper_slots);
            LOG_ERROR(">>> PLEASE LOAD PAPER INTO THE PRINTER TRAY AND ENSURE IT IS FULLY INSERTED <<<");
            return -2;
        }
        if (!st.cover_closed) {
            LOG_ERROR("Cannot print page %u: PRINTER COVER IS OPEN", printer->page_counter + 1);
            return -3;
        }
    }

    /* Record starting status before printing to track page counter */
    capt_status_t init_status;
    memset(&init_status, 0, sizeof(init_status));
    get_extended_status(printer, &init_status);
    uint16_t start_printed = init_status.page_printed;
    LOG_INFO("Starting page (job page %u): initial printer printed=%u, start=%u",
             printer->page_counter + 1, start_printed, init_status.page_start);

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
    
    /* Phase 2: Send page parameters (34-byte payload) */
    uint8_t pdata[34] = {0};
    pdata[0] = 0x00; pdata[1] = 0x00;
    pdata[2] = params->target_model & 0xFF; pdata[3] = (params->target_model >> 8) & 0xFF;
    pdata[4] = params->paper_size; pdata[5] = params->media_source;
    pdata[6] = params->input_slot; pdata[7] = 0x00;
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
    LOG_INFO("Page %u raster sent, waiting for printer engine to deliver...", page_num);
    
    /* Phase 6: Wait for page delivery (up to 60 seconds) */
    int timeout = 60000;
    int elapsed = 0;
    capt_status_t status;
    bool delivered = false;

    /* Physical laser pickup and heating cycle takes at least 3-4 seconds */
    SLEEP_MS(3000);
    elapsed += 3000;

    do {
        if (get_extended_status(printer, &status) == 0) {
            LOG_DEBUG("Delivery status (t=%ds): printed=%u (start=%u), start_cnt=%u, printing=%u, shipped=%u, engine=0x%04X, basic=0x%02X, aux=0x%02X",
                      elapsed / 1000, status.page_printed, start_printed,
                      status.page_start, status.page_printing, status.page_shipped,
                      status.engine, status.basic, status.aux);

            if (status.page_printed > start_printed) {
                LOG_INFO("Page %u printed and ejected successfully (counter %u -> %u)",
                         page_num, start_printed, status.page_printed);
                delivered = true;
                break;
            }
            if (status.error) {
                LOG_ERROR("Printer error during page printing: %s (engine=0x%04X, basic=0x%02X)",
                          status.error_string, status.engine, status.basic);
                break;
            }
            /* If 12 seconds have elapsed and the engine is idle and ready */
            if (elapsed >= 12000 && (status.basic & 0x02) == 0 && (status.aux & 0x06) == 0) {
                LOG_INFO("Printer engine cycle completed (aux=0x%02X, basic=0x%02X)",
                         status.aux, status.basic);
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
    return delivered ? 0 : -1;
}

