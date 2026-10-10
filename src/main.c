/*
 * main.c - Canon LBP-810 CAPT Print Service entry point
 *
 * Runs as a Windows service or in console mode (--console flag).
 * Coordinates the IPP server, CAPT protocol engine, and USB communication.
 */
#include "platform.h"
#include "log.h"
#include "ipp_server.h"
#include "capt.h"
#include "pwg_raster.h"
#include "dither.h"
#include "scoa.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

/* ── Global state ────────────────────────────────────── */
static volatile bool      g_stop = false;
static capt_printer_t     g_capt_dev;
static bool               g_capt_connected = false;
static uint16_t           g_port = 6631;

#ifdef _WIN32
static SERVICE_STATUS_HANDLE g_svc_handle = NULL;
static SERVICE_STATUS        g_svc_status;
#endif

static platform_mutex_t   g_capt_mutex;
static volatile bool      g_job_cancelled = false;
static volatile bool      g_is_printing    = false;
static int                g_printer_state  = 3; // 3 = idle, 4 = processing, 5 = stopped
static const char        *g_printer_state_reasons = "none";
static bool               g_printer_accepting_jobs = true;

static dither_algorithm_t g_dither_algo = DITHER_ADAPTIVE;
static uint8_t            g_toner_density = 0x1F;
static bool               g_force_toner_save = false;
static uint8_t            g_force_paper_code = PAPER_A4; // Default to ISO A4 for Canon LBP-810 (or 0 for auto-detect)

/* ── Build CAPT page params from PWG header ── */
static void build_page_params(capt_page_params_t *params,
                              const pwg_page_header_t *hdr)
{
    uint32_t dpi = hdr->hw_resolution_x ? hdr->hw_resolution_x : 600;

    /* Paper format selection: priority to forced/default --paper option if set */
    const capt_paper_info_t *paper = NULL;
    if (g_force_paper_code != 0) {
        paper = capt_get_paper_info_by_code(g_force_paper_code);
    } else {
        paper = capt_get_paper_info_by_name(hdr->page_size_name);
        if (!paper) {
            paper = capt_get_paper_info_by_dims(hdr->width, hdr->height, dpi);
        }
    }
    if (!paper) {
        paper = capt_get_paper_info_by_code(PAPER_A4);
    }

    /* Input Slot selection (Roadmap 2.2) */
    uint8_t input_slot = 0x01; // Default: Main auto cassette
    if (hdr->manual_feed != 0 ||
        paper->code == PAPER_ENV_COM10 || paper->code == PAPER_ENV_DL || paper->code == PAPER_ENV_C5 ||
        hdr->media_position == 2 /* manual tray in standard PWG */ ||
        strstr(hdr->media_type, "manual") != NULL) {
        input_slot = 0x00; // Manual Feed Slot
        LOG_INFO("Selected manual feed slot (input_slot=0x00) for paper '%s'", paper->name);
        LOG_INFO("Waiting for sheet in manual feed slot...");
    } else {
        LOG_INFO("Selected main cassette (input_slot=0x01) for paper '%s'", paper->name);
    }

    /* Print quality & Toner saving (Roadmap 2.3) */
    uint8_t toner_saving = g_force_toner_save ? 0x01 : 0x00;
    uint8_t smoothing = 0x02; // ON by default
    if (strstr(hdr->rendering_intent, "draft") != NULL ||
        hdr->pwg_integer[0] == 3 /* draft quality */) {
        toner_saving = 0x01;
        smoothing = 0x00;
        LOG_INFO("Draft / Toner saving mode activated");
    }

    capt_build_page_params(params, paper->code, dpi, input_slot, toner_saving, smoothing);
    memset(params->toner_density, g_toner_density, 4);
    LOG_INFO("Built page params: %s (PWG: %ux%u px, name='%s', linesize=%u, lines=%u, %u DPI, slot=0x%02X, toner_save=0x%02X%s)",
             paper->name, hdr->width, hdr->height, hdr->page_size_name,
             params->image_line_size, params->image_lines, dpi,
             params->input_slot, params->toner_saving,
             (g_force_paper_code != 0) ? " [A4 default/forced]" : "");
}

/* ── Cancel callback: called from IPP thread when Cancel-Job received ── */
static void on_cancel_job(void *user_data)
{
    (void)user_data;
    LOG_WARN("Cancel-Job request received from spooler");
    g_job_cancelled = true;
}

/* ── Status callback: called when IPP client queries printer attributes ── */
static void get_ipp_status(ipp_printer_state_info_t *info, void *user_data)
{
    (void)user_data;
    if (g_is_printing) {
        info->printer_state = g_printer_state;
        info->state_reasons = g_printer_state_reasons;
        info->is_accepting_jobs = g_printer_accepting_jobs;
        return;
    }

    platform_mutex_lock(&g_capt_mutex);

    if (!g_capt_connected) {
        if (capt_open(&g_capt_dev) == 0) {
            g_capt_connected = true;
            g_capt_dev.cancel_flag = &g_job_cancelled;
        }
    }

    if (g_capt_connected) {
        capt_status_t st;
        uint8_t raw[256];
        uint16_t actual = 0;
        if (usb_send_packet(&g_capt_dev.usb, CAPT_GET_EXTENDED_STATUS, NULL, 0) == 0 &&
            usb_recv_packet(&g_capt_dev.usb, CAPT_GET_EXTENDED_STATUS, raw, sizeof(raw), &actual) == 0 &&
            actual >= 16) {
            capt_parse_status(raw, actual, &st);
            if (!st.cover_closed) {
                g_printer_state = 5; // stopped
                g_printer_state_reasons = "door-open-error";
                g_printer_accepting_jobs = false;
            } else if (!st.cartridge_present) {
                g_printer_state = 5; // stopped
                g_printer_state_reasons = "marker-supply-missing-error";
                g_printer_accepting_jobs = false;
            } else if ((st.basic & 0x80) && (st.engine & 0x0200)) {
                g_printer_state = 5; // stopped
                g_printer_state_reasons = "media-empty-error";
                g_printer_accepting_jobs = false;
            } else if ((st.basic & 0x80) && (st.engine & 0x0100)) {
                g_printer_state = 5; // stopped
                g_printer_state_reasons = "media-jam-error";
                g_printer_accepting_jobs = false;
            } else if (st.error) {
                g_printer_state = 5; // stopped
                g_printer_state_reasons = "other";
                g_printer_accepting_jobs = false;
            } else {
                g_printer_state = 3; // idle
                g_printer_state_reasons = "none";
                g_printer_accepting_jobs = true;
            }
        } else {
            /* USB error -> mark disconnected so next attempt re-probes */
            capt_close(&g_capt_dev);
            g_capt_connected = false;
            g_printer_state = 3;
            g_printer_state_reasons = "none";
            g_printer_accepting_jobs = true;
        }
    } else {
        g_printer_state = 3;
        g_printer_state_reasons = "none";
        g_printer_accepting_jobs = true;
    }

    info->printer_state = g_printer_state;
    info->state_reasons = g_printer_state_reasons;
    info->is_accepting_jobs = g_printer_accepting_jobs;

    platform_mutex_unlock(&g_capt_mutex);
}

/* ── Print callback: receives PWG-Raster from IPP, drives CAPT ── */
static void on_print_job(const uint8_t *pwg_data, size_t pwg_len, void *user_data)
{
    (void)user_data;
    LOG_INFO("Received print job: %zu bytes of PWG-Raster data", pwg_len);

    platform_mutex_lock(&g_capt_mutex);

    g_job_cancelled = false;
    g_is_printing = true;
    g_printer_state = 4; // processing
    g_printer_state_reasons = "none";
    g_printer_accepting_jobs = false;

    /* Open CAPT device if not already connected (with reconnect retries) */
    if (!g_capt_connected) {
        int retries = 3;
        while (retries-- > 0 && !g_capt_connected) {
            if (capt_open(&g_capt_dev) == 0) {
                g_capt_connected = true;
                break;
            }
            platform_sleep_ms(500);
        }
        if (!g_capt_connected) {
            LOG_ERROR("Cannot open Canon LBP-810 USB device");
            g_is_printing = false;
            g_printer_state = 3;
            g_printer_accepting_jobs = true;
            platform_mutex_unlock(&g_capt_mutex);
            return;
        }
    }
    g_capt_dev.cancel_flag = &g_job_cancelled;

    /* Parse PWG-Raster stream */
    pwg_stream_t stream;
    if (pwg_open(&stream, pwg_data, pwg_len) != 0) {
        LOG_ERROR("Invalid PWG-Raster data (bad magic)");
        g_is_printing = false;
        g_printer_state = 3;
        g_printer_accepting_jobs = true;
        platform_mutex_unlock(&g_capt_mutex);
        return;
    }

    /* Begin print job */
    if (capt_job_begin(&g_capt_dev) != 0) {
        LOG_ERROR("Failed to begin CAPT print job");
        capt_close(&g_capt_dev);
        g_capt_connected = false;
        g_is_printing = false;
        g_printer_state = 3;
        g_printer_accepting_jobs = true;
        platform_mutex_unlock(&g_capt_mutex);
        return;
    }

    int page_num = 0;
    pwg_page_header_t hdr;

    while (pwg_read_page_header(&stream, &hdr) == 0) {
        if (g_job_cancelled) {
            LOG_WARN("Job cancelled before page %d header processed", page_num + 1);
            break;
        }

        page_num++;
        LOG_INFO("Processing page %d: %ux%u pixels, %u DPI, %u bpp",
                 page_num, hdr.width, hdr.height, hdr.hw_resolution_x, hdr.bits_per_pixel);

        /* Build CAPT page parameters */
        capt_page_params_t params;
        build_page_params(&params, &hdr);

        uint32_t mono_line_bytes = params.image_line_size;
        uint32_t mono_total = (uint32_t)mono_line_bytes * params.image_lines;
        uint8_t *mono_bitmap = (uint8_t *)calloc(mono_total, 1);
        if (!mono_bitmap) {
            LOG_ERROR("Out of memory for page bitmap (%u bytes)", mono_total);
            break;
        }

        uint32_t gray_line_bytes = hdr.bytes_per_line;
        uint8_t *gray_line_buf = (uint8_t *)malloc(gray_line_bytes);
        if (!gray_line_buf) {
            LOG_ERROR("Out of memory for grayscale line buffer");
            free(mono_bitmap);
            break;
        }

        /* Initialize dithering */
        uint32_t pixel_width = mono_line_bytes * 8;
        if (pixel_width > hdr.width)
            pixel_width = hdr.width;

        dither_ctx_t dither;
        dither_init(&dither, (int)pixel_width);

        /* Calculate margin crop offsets from full-page raster */
        uint32_t left_offset = 0;
        if (hdr.width > pixel_width && params.margin_left <= (hdr.width - pixel_width)) {
            left_offset = params.margin_left;
        }
        uint32_t top_offset = 0;
        if (hdr.height > params.image_lines && params.margin_top <= (hdr.height - params.image_lines)) {
            top_offset = params.margin_top;
        }

        /* Decode each PWG scanline and apply requested halftoning */
        uint32_t y = 0;
        while (y < hdr.height && stream.offset < stream.data_len) {
            if (g_job_cancelled) {
                LOG_WARN("Job cancelled during raster decode at line %u", y);
                break;
            }

            uint32_t line_repeat = 0;
            if (pwg_read_line(&stream, &hdr, gray_line_buf, &line_repeat) != 0) {
                LOG_WARN("PWG-Raster decode error at line %u", y);
                break;
            }

            for (uint32_t r = 0; r < line_repeat && y < hdr.height; r++, y++) {
                if (y >= top_offset && (y - top_offset) < params.image_lines) {
                    uint8_t *mono_row = mono_bitmap + ((y - top_offset) * mono_line_bytes);
                    if (hdr.bits_per_pixel == 1) {
                        bool invert = (hdr.color_space == 18 /* sGray */ || hdr.color_space == 3);
                        for (uint32_t b = 0; b < mono_line_bytes; b++) {
                            uint32_t src_idx = left_offset / 8 + b;
                            uint8_t byte = (src_idx < gray_line_bytes) ? gray_line_buf[src_idx] : (invert ? 0xFF : 0x00);
                            mono_row[b] = invert ? ~byte : byte;
                        }
                    } else {
                        dither_render_line(&dither, gray_line_buf + left_offset, mono_row, (int)y, g_dither_algo);
                    }
                }
            }
        }

        LOG_INFO("Decoded %u of %u scanlines for page %d", y, hdr.height, page_num);

        free(gray_line_buf);
        dither_free(&dither);

        if (g_job_cancelled) {
            free(mono_bitmap);
            break;
        }

        /* Send page to printer via CAPT */
        LOG_INFO("Sending page %d to printer (%u x %u, %u DPI)...",
                 page_num, mono_line_bytes * 8, params.image_lines,
                 hdr.hw_resolution_x);

        int rc = capt_print_page(&g_capt_dev, &params,
                                 mono_bitmap, mono_line_bytes,
                                 params.image_lines);
        free(mono_bitmap);

        if (rc == CAPT_ERR_CANCELLED) {
            LOG_WARN("Page %d printing cancelled by user", page_num);
            break;
        } else if (rc == CAPT_ERR_NO_PAPER) {
            LOG_ERROR("Page %d printing halted: Out of paper", page_num);
            g_printer_state = 5;
            g_printer_state_reasons = "media-empty-error";
            break;
        } else if (rc == CAPT_ERR_COVER_OPEN) {
            LOG_ERROR("Page %d printing halted: Cover open", page_num);
            g_printer_state = 5;
            g_printer_state_reasons = "door-open-error";
            break;
        } else if (rc == CAPT_ERR_JAM) {
            LOG_ERROR("Page %d printing halted: Paper jam", page_num);
            g_printer_state = 5;
            g_printer_state_reasons = "media-jam-error";
            break;
        } else if (rc == CAPT_ERR_BUFFER_TIMEOUT) {
            LOG_ERROR("Page %d printing halted: Printer buffer timeout (engine stalled)", page_num);
            break;
        } else if (rc == CAPT_ERR_DELIVERY_TIMEOUT) {
            LOG_WARN("Page %d delivery confirmation timed out (data transmitted successfully)", page_num);
            /* Do not abort remaining pages of a multi-page job */
            if (pwg_has_more_pages(&stream)) {
                LOG_INFO("Proceeding to next page of job...");
                platform_sleep_ms(1000);
            }
        } else if (rc != CAPT_OK) {
            LOG_ERROR("Failed to print page %d: communication error %d", page_num, rc);
            /* Reset USB only on hardware communication failure */
            capt_close(&g_capt_dev);
            g_capt_connected = false;
            break;
        }

        LOG_INFO("Page %d printed successfully", page_num);
        if (pwg_has_more_pages(&stream)) {
            LOG_INFO("Proceeding to next page of job...");
            platform_sleep_ms(500);
        }
    }

    /* End or cancel print job */
    if (g_job_cancelled) {
        LOG_WARN("Executing CAPT cancel and buffer purge...");
        capt_cancel_job(&g_capt_dev);
        g_printer_state = 3;
        g_printer_state_reasons = "none";
        g_printer_accepting_jobs = true;
    } else {
        capt_job_end(&g_capt_dev);
        if (g_printer_state == 4) {
            g_printer_state = 3;
            g_printer_state_reasons = "none";
            g_printer_accepting_jobs = true;
        }
    }
    LOG_INFO("Print job finished: %d page(s)", page_num);
    g_is_printing = false;

    platform_mutex_unlock(&g_capt_mutex);
}

/* ── Server main loop ─────────────────────────────────── */
static int run_server(void)
{
    platform_mutex_init(&g_capt_mutex);

    /* Try to open the CAPT device at startup */
    if (capt_open(&g_capt_dev) == 0) {
        g_capt_connected = true;
        g_capt_dev.cancel_flag = &g_job_cancelled;
        LOG_INFO("Canon LBP-810 found and connected via USB");
    } else {
        LOG_WARN("Canon LBP-810 not found — will retry when print job arrives");
    }

    ipp_server_config_t config = {
        .listen_port  = g_port,
        .printer_name = "Canon LBP-810",
        .on_print     = on_print_job,
        .on_cancel    = on_cancel_job,
        .get_status   = get_ipp_status,
        .user_data    = NULL,
        .stop_flag    = &g_stop
    };

    LOG_INFO("Starting IPP server on port %u...", config.listen_port);
    int rc = ipp_server_run(&config);

    /* Cleanup */
    if (g_capt_connected) {
        capt_close(&g_capt_dev);
        g_capt_connected = false;
    }

    platform_mutex_destroy(&g_capt_mutex);
    return rc;
}

/* ── Signal handler for console mode ─────────────────── */
static void signal_handler(int sig)
{
    (void)sig;
    LOG_INFO("Shutdown signal received");
    g_stop = true;
    ipp_server_stop();
}

/* ── Windows Service callbacks ───────────────────────── */
#ifdef _WIN32

static void WINAPI service_ctrl_handler(DWORD ctrl)
{
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        g_svc_status.dwCurrentState = SERVICE_STOP_PENDING;
        g_svc_status.dwWaitHint = 5000;
        SetServiceStatus(g_svc_handle, &g_svc_status);
        g_stop = true;
        ipp_server_stop();
        break;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(g_svc_handle, &g_svc_status);
        break;
    default:
        break;
    }
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv)
{
    (void)argc; (void)argv;

    g_svc_handle = RegisterServiceCtrlHandlerW(L"CaptLBP810",
                                                service_ctrl_handler);
    if (!g_svc_handle) return;

    memset(&g_svc_status, 0, sizeof(g_svc_status));
    g_svc_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_svc_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_svc_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_svc_handle, &g_svc_status);

    log_init(LOG_TARGET_FILE, "C:\\ProgramData\\CaptLBP810\\capt-service.log");
    LOG_INFO("Canon LBP-810 CAPT service starting");

    run_server();

    g_svc_status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_svc_handle, &g_svc_status);
    log_shutdown();
}

#endif /* _WIN32 */

/* ── Entry point ─────────────────────────────────────── */
int main(int argc, char *argv[])
{
    bool console_mode = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0 || strcmp(argv[i], "-c") == 0) {
            console_mode = true;
        } else if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc) {
            g_port = (uint16_t)atoi(argv[++i]);
        } else if ((strcmp(argv[i], "--dither") == 0 || strcmp(argv[i], "-d") == 0) && i + 1 < argc) {
            const char *algo = argv[++i];
            if (strcmp(algo, "adaptive") == 0) g_dither_algo = DITHER_ADAPTIVE;
            else if (strcmp(algo, "fs") == 0 || strcmp(algo, "floyd-steinberg") == 0) g_dither_algo = DITHER_FLOYD_STEINBERG;
            else if (strcmp(algo, "atkinson") == 0) g_dither_algo = DITHER_ATKINSON;
            else if (strcmp(algo, "bayer") == 0 || strcmp(algo, "bayer8x8") == 0) g_dither_algo = DITHER_BAYER_8X8;
            else if (strcmp(algo, "threshold") == 0) g_dither_algo = DITHER_THRESHOLD;
        } else if (strcmp(argv[i], "--density") == 0 && i + 1 < argc) {
            int d = atoi(argv[++i]);
            if (d < 1) d = 1;
            if (d > 5) d = 5;
            static const uint8_t levels[5] = { 0x06, 0x0C, 0x13, 0x19, 0x1F };
            g_toner_density = levels[d - 1];
        } else if (strcmp(argv[i], "--toner-save") == 0) {
            g_force_toner_save = true;
        } else if ((strcmp(argv[i], "--paper") == 0 || strcmp(argv[i], "-P") == 0) && i + 1 < argc) {
            const char *pname = argv[++i];
            if (strcmp(pname, "auto") == 0 || strcmp(pname, "0") == 0) {
                g_force_paper_code = 0;
                LOG_INFO("Paper format selection: auto-detect from print job");
            } else {
                const capt_paper_info_t *pinfo = capt_get_paper_info_by_name(pname);
                if (pinfo) {
                    g_force_paper_code = pinfo->code;
                    LOG_INFO("Default paper format set to: %s (code 0x%02X)", pinfo->name, pinfo->code);
                } else {
                    LOG_WARN("Unknown paper format '%s', ignoring", pname);
                }
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Canon LBP-810 CAPT Print Service\n\n");
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --console, -c          Run in console mode (foreground)\n");
            printf("  --port, -p <PORT>      IPP listen port (default: 6631)\n");
            printf("  --paper, -P <NAME>     Default paper format (default: A4; or auto, Letter, Legal, etc.)\n");
            printf("  --dither, -d <ALGO>    Dithering: adaptive, fs, atkinson, bayer, threshold\n");
            printf("  --density <1-5>        Toner density level (1=lightest, 5=darkest)\n");
            printf("  --toner-save           Force toner saving draft mode\n");
            printf("  --help, -h             Show this help\n");
            printf("\nWhen run without --console, starts as a Windows service.\n");
            printf("Use install/install.ps1 to install the service.\n");
            return 0;
        }
    }

#ifndef _WIN32
    console_mode = true;
#endif

    if (console_mode) {
        log_init(LOG_TARGET_CONSOLE, NULL);
        log_set_level(LOG_LEVEL_DEBUG);

        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        printf("Canon LBP-810 CAPT Print Service — Console Mode\n");
        printf("Press Ctrl+C to stop.\n\n");

        int rc = run_server();
        log_shutdown();
        return rc;
    }

#ifdef _WIN32
    SERVICE_TABLE_ENTRYW table[] = {
        { L"CaptLBP810", service_main },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            printf("Not started by Service Control Manager.\n");
            printf("Use --console flag for foreground mode, or install as service.\n");
            return 1;
        }
        return (int)err;
    }
#endif

    return 0;
}
