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
static volatile bool g_stop = false;
static capt_printer_t g_capt_dev;
static bool           g_capt_connected = false;
static uint16_t       g_port = 6631;

#ifdef _WIN32
static SERVICE_STATUS_HANDLE g_svc_handle = NULL;
static SERVICE_STATUS        g_svc_status;
#endif

/* ── Build CAPT page params from PWG header ── */
static void build_page_params(capt_page_params_t *params,
                              const pwg_page_header_t *hdr)
{
    memset(params, 0, sizeof(*params));
    params->target_model = CAPT_MODEL_LBP810; /* 0x01A4 */
    params->media_source = 0x01;
    params->input_slot   = 0x01; /* 0x01 = Main Tray / Auto Cassette (0x00 is Manual Feed) */
    memset(params->toner_density, 0x1F, 4); /* Maximum density */
    params->mode         = 0x00;
    params->resolution   = (hdr->hw_resolution_x >= 600) ? 0x11 : 0x00;
    params->constants[0] = 0x03;
    params->constants[1] = 0x01;
    params->constants[2] = 0x01;
    params->constants[3] = 0x01;
    params->smoothing    = 0x02; /* ON */
    params->toner_saving = 0x00; /* OFF */

    uint32_t dpi = hdr->hw_resolution_x ? hdr->hw_resolution_x : 600;
    bool is_letter = (hdr->width > 5000) || (dpi < 600 && hdr->width > 2500) ||
                     (strstr(hdr->page_size_name, "letter") != NULL);

    if (is_letter) {
        params->paper_size = PAPER_LETTER; /* 0x0D */
        if (dpi >= 600) {
            params->paper_width     = 5100;
            params->paper_height    = 6600;
            params->margin_left     = 110;
            params->margin_top      = 120;
            params->image_line_size = 610;  /* 4880 pixels = (5100 - 2*110) */
            params->image_lines     = 6360;  /* 6600 - 2*120 */
        } else {
            params->paper_width     = 2550;
            params->paper_height    = 3300;
            params->margin_left     = 55;
            params->margin_top      = 60;
            params->image_line_size = 305;
            params->image_lines     = 3180;
        }
    } else {
        params->paper_size = PAPER_A4; /* 0x02 */
        if (dpi >= 600) {
            params->paper_width     = 4960;
            params->paper_height    = 7014;
            params->margin_left     = 112;
            params->margin_top      = 119;
            params->image_line_size = 592;  /* (4960 - 2*112) / 8 */
            params->image_lines     = 6776;  /* 7014 - 2*119 */
        } else {
            params->paper_width     = 2480;
            params->paper_height    = 3507;
            params->margin_left     = 56;
            params->margin_top      = 60;
            params->image_line_size = 296;
            params->image_lines     = 3388;
        }
    }
}

/* ── Print callback: receives PWG-Raster from IPP, drives CAPT ── */
static void on_print_job(const uint8_t *pwg_data, size_t pwg_len, void *user_data)
{
    (void)user_data;
    LOG_INFO("Received print job: %zu bytes of PWG-Raster data", pwg_len);

    /* Open CAPT device if not already connected */
    if (!g_capt_connected) {
        if (capt_open(&g_capt_dev) != 0) {
            LOG_ERROR("Cannot open Canon LBP-810 USB device");
            return;
        }
        g_capt_connected = true;
    }

    /* Parse PWG-Raster stream */
    pwg_stream_t stream;
    if (pwg_open(&stream, pwg_data, pwg_len) != 0) {
        LOG_ERROR("Invalid PWG-Raster data (bad magic)");
        return;
    }

    /* Begin print job */
    if (capt_job_begin(&g_capt_dev) != 0) {
        LOG_ERROR("Failed to begin CAPT print job");
        return;
    }

    int page_num = 0;
    pwg_page_header_t hdr;

    while (pwg_read_page_header(&stream, &hdr) == 0) {
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

        /* Initialize Floyd-Steinberg dithering */
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

        /* Decode each PWG scanline (with line repetitions) and dither to 1bpp */
        uint32_t y = 0;
        while (y < hdr.height && stream.offset < stream.data_len) {
            uint32_t line_repeat = 0;
            if (pwg_read_line(&stream, &hdr, gray_line_buf, &line_repeat) != 0) {
                LOG_WARN("PWG-Raster decode error at line %u", y);
                break;
            }

            for (uint32_t r = 0; r < line_repeat && y < hdr.height; r++, y++) {
                if (y >= top_offset && (y - top_offset) < params.image_lines) {
                    uint8_t *mono_row = mono_bitmap + ((y - top_offset) * mono_line_bytes);
                    if (hdr.bits_per_pixel == 1) {
                        bool invert = (hdr.color_space == 18 /* sGray */);
                        for (uint32_t b = 0; b < mono_line_bytes; b++) {
                            uint8_t byte = gray_line_buf[left_offset / 8 + b];
                            mono_row[b] = invert ? ~byte : byte;
                        }
                    } else {
                        dither_line(&dither, gray_line_buf + left_offset, mono_row);
                    }
                }
            }
        }

        LOG_INFO("Decoded %u of %u scanlines for page %d", y, hdr.height, page_num);

        free(gray_line_buf);
        dither_free(&dither);

        /* Send page to printer via CAPT */
        LOG_INFO("Sending page %d to printer (%u x %u, %u DPI)...",
                 page_num, mono_line_bytes * 8, params.image_lines,
                 hdr.hw_resolution_x);

        int rc = capt_print_page(&g_capt_dev, &params,
                                 mono_bitmap, mono_line_bytes,
                                 params.image_lines);
        free(mono_bitmap);

        if (rc != 0) {
            LOG_ERROR("Failed to print page %d: error %d", page_num, rc);
            break;
        }

        LOG_INFO("Page %d printed successfully", page_num);
    }

    /* End print job */
    capt_job_end(&g_capt_dev);
    LOG_INFO("Print job complete: %d page(s)", page_num);
}

/* ── Server main loop ─────────────────────────────────── */
static int run_server(void)
{
    /* Try to open the CAPT device at startup */
    if (capt_open(&g_capt_dev) == 0) {
        g_capt_connected = true;
        LOG_INFO("Canon LBP-810 found and connected via USB");
    } else {
        LOG_WARN("Canon LBP-810 not found — will retry when print job arrives");
    }

    ipp_server_config_t config = {
        .listen_port  = g_port,
        .printer_name = "Canon LBP-810",
        .on_print     = on_print_job,
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
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Canon LBP-810 CAPT Print Service\n\n");
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  --console, -c          Run in console mode (foreground)\n");
            printf("  --port, -p <PORT>      IPP listen port (default: 6631)\n");
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
