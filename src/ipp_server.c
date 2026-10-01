#include "ipp_server.h"
#include "platform.h"
#include "log.h"
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#define strncasecmp _strnicmp
#define strcasecmp  _stricmp
#endif

#define IPP_OP_PRINT_JOB               0x0002
#define IPP_OP_VALIDATE_JOB            0x0004
#define IPP_OP_CANCEL_JOB              0x0008
#define IPP_OP_GET_JOB_ATTRIBUTES      0x0009
#define IPP_OP_GET_JOBS                0x000A
#define IPP_OP_GET_PRINTER_ATTRIBUTES  0x000B

#define IPP_STATUS_OK                  0x0000
#define IPP_STATUS_SERVER_ERROR        0x0500
#define IPP_STATUS_OP_NOT_SUPPORTED    0x0400

#define IPP_TAG_OPERATION_ATTRIBUTES   0x01
#define IPP_TAG_JOB_ATTRIBUTES         0x02
#define IPP_TAG_END_OF_ATTRIBUTES      0x03
#define IPP_TAG_PRINTER_ATTRIBUTES     0x04

#define IPP_TAG_INTEGER                0x21
#define IPP_TAG_BOOLEAN                0x22
#define IPP_TAG_ENUM                   0x23
#define IPP_TAG_DATETIME               0x31
#define IPP_TAG_RESOLUTION             0x33
#define IPP_TAG_RANGE_OF_INTEGER       0x32
#define IPP_TAG_TEXT_WITHOUT_LANGUAGE  0x41
#define IPP_TAG_NAME_WITHOUT_LANGUAGE  0x42
#define IPP_TAG_KEYWORD                0x44
#define IPP_TAG_URI                    0x45
#define IPP_TAG_CHARSET                0x47
#define IPP_TAG_NATURAL_LANGUAGE       0x48
#define IPP_TAG_MIME_MEDIA_TYPE        0x49

static volatile bool g_stop_server = false;
static uint32_t g_startup_time = 0;

void ipp_server_stop(void) {
    g_stop_server = true;
}

// Write utilities
static void write_u8(uint8_t **buf, uint8_t val) {
    *(*buf)++ = val;
}
static void write_u16(uint8_t **buf, uint16_t val) {
    *(*buf)++ = (val >> 8) & 0xFF;
    *(*buf)++ = val & 0xFF;
}
static void write_u32(uint8_t **buf, uint32_t val) {
    *(*buf)++ = (val >> 24) & 0xFF;
    *(*buf)++ = (val >> 16) & 0xFF;
    *(*buf)++ = (val >> 8) & 0xFF;
    *(*buf)++ = val & 0xFF;
}
static void write_bytes(uint8_t **buf, const void *data, size_t len) {
    if (len > 0) {
        memcpy(*buf, data, len);
        *buf += len;
    }
}

static void write_attr_header(uint8_t **buf, uint8_t tag, const char *name, uint16_t value_len) {
    write_u8(buf, tag);
    uint16_t name_len = name ? (uint16_t)strlen(name) : 0;
    write_u16(buf, name_len);
    if (name_len > 0) {
        write_bytes(buf, name, name_len);
    }
    write_u16(buf, value_len);
}

static void write_attr_int(uint8_t **buf, const char *name, int32_t value, uint8_t tag) {
    write_attr_header(buf, tag, name, 4);
    write_u32(buf, (uint32_t)value);
}

static void write_attr_str(uint8_t **buf, const char *name, const char *value, uint8_t tag) {
    uint16_t len = (uint16_t)strlen(value);
    write_attr_header(buf, tag, name, len);
    write_bytes(buf, value, len);
}

static void write_attr_bool(uint8_t **buf, const char *name, bool value) {
    write_attr_header(buf, IPP_TAG_BOOLEAN, name, 1);
    write_u8(buf, value ? 0x01 : 0x00);
}

static void write_attr_resolution(uint8_t **buf, const char *name, uint32_t xres, uint32_t yres, uint8_t units) {
    write_attr_header(buf, IPP_TAG_RESOLUTION, name, 9);
    write_u32(buf, xres);
    write_u32(buf, yres);
    write_u8(buf, units);
}

static void write_attr_range(uint8_t **buf, const char *name, int32_t lower, int32_t upper) {
    write_attr_header(buf, IPP_TAG_RANGE_OF_INTEGER, name, 8);
    write_u32(buf, (uint32_t)lower);
    write_u32(buf, (uint32_t)upper);
}

// IPP Request Handler
static void handle_ipp_request(const uint8_t *req, size_t req_len, uint8_t **resp_buf, size_t *resp_len, const ipp_server_config_t *config) {
    if (req_len < 8) return; // Too small

    uint16_t version = (req[0] << 8) | req[1];
    uint16_t op_id = (req[2] << 8) | req[3];
    uint32_t req_id = (req[4] << 24) | (req[5] << 16) | (req[6] << 8) | req[7];

    LOG_DEBUG("IPP Request: op=0x%04x, req_id=%u, version=0x%04x", op_id, req_id, version);

    // Prepare response buffer
    uint8_t *res = malloc(65536);
    if (!res) return;
    uint8_t *p = res;

    // Response header - echo client version (IPP 1.0, 1.1, 2.0)
    uint16_t resp_version = (version >= 0x0100 && version <= 0x0200) ? version : 0x0101;

    // If unsupported operation or private vendor opcode (>= 0x4000)
    if (op_id >= 0x4000 || (op_id != IPP_OP_GET_PRINTER_ATTRIBUTES &&
                            op_id != IPP_OP_PRINT_JOB &&
                            op_id != IPP_OP_VALIDATE_JOB &&
                            op_id != IPP_OP_CANCEL_JOB &&
                            op_id != IPP_OP_GET_JOB_ATTRIBUTES &&
                            op_id != IPP_OP_GET_JOBS)) {
        LOG_DEBUG("Unsupported IPP operation 0x%04x -> returning 0x0501 (not supported)", op_id);
        write_u16(&p, resp_version);
        write_u16(&p, 0x0501); // server-error-operation-not-supported
        write_u32(&p, req_id);
        write_u8(&p, IPP_TAG_OPERATION_ATTRIBUTES);
        write_attr_str(&p, "attributes-charset", "utf-8", IPP_TAG_CHARSET);
        write_attr_str(&p, "attributes-natural-language", "en", IPP_TAG_NATURAL_LANGUAGE);
        write_u8(&p, IPP_TAG_END_OF_ATTRIBUTES);
        *resp_buf = res;
        *resp_len = p - res;
        return;
    }

    write_u16(&p, resp_version);
    write_u16(&p, IPP_STATUS_OK);
    write_u32(&p, req_id);

    // Common Operation Attributes Group
    write_u8(&p, IPP_TAG_OPERATION_ATTRIBUTES);
    write_attr_str(&p, "attributes-charset", "utf-8", IPP_TAG_CHARSET);
    write_attr_str(&p, "attributes-natural-language", "en", IPP_TAG_NATURAL_LANGUAGE);

    if (op_id == IPP_OP_GET_PRINTER_ATTRIBUTES) {
        write_u8(&p, IPP_TAG_PRINTER_ATTRIBUTES);
        
        char printer_uri[128];
        uint16_t port = config->listen_port ? config->listen_port : 6631;
        snprintf(printer_uri, sizeof(printer_uri), "ipp://localhost:%u/printers/canon-lbp810", port);
        write_attr_str(&p, "printer-uri-supported", printer_uri, IPP_TAG_URI);
        write_attr_str(&p, "uri-security-supported", "none", IPP_TAG_KEYWORD);
        write_attr_str(&p, "uri-authentication-supported", "none", IPP_TAG_KEYWORD);
        write_attr_str(&p, "printer-name", config->printer_name, IPP_TAG_NAME_WITHOUT_LANGUAGE);
        write_attr_str(&p, "printer-info", config->printer_name, IPP_TAG_TEXT_WITHOUT_LANGUAGE);
        write_attr_str(&p, "printer-device-id", "MFG:Canon;MDL:LBP-810;CMD:PWG,URF;CLS:PRINTER;DES:Canon LBP-810;", IPP_TAG_TEXT_WITHOUT_LANGUAGE);
        write_attr_str(&p, "printer-uuid", "urn:uuid:b3e84dd6-9c49-4331-97c7-7a8e37391def", IPP_TAG_URI);
        write_attr_int(&p, "printer-state", 3, IPP_TAG_ENUM); // 3 = idle
        write_attr_str(&p, "printer-state-reasons", "none", IPP_TAG_KEYWORD);
        write_attr_str(&p, "ipp-versions-supported", "1.1", IPP_TAG_KEYWORD);
        write_attr_str(&p, NULL, "2.0", IPP_TAG_KEYWORD);
        write_attr_str(&p, NULL, "1.0", IPP_TAG_KEYWORD);
        
        write_attr_int(&p, "operations-supported", IPP_OP_PRINT_JOB, IPP_TAG_ENUM);
        write_attr_int(&p, NULL, IPP_OP_VALIDATE_JOB, IPP_TAG_ENUM);
        write_attr_int(&p, NULL, IPP_OP_CANCEL_JOB, IPP_TAG_ENUM);
        write_attr_int(&p, NULL, IPP_OP_GET_JOB_ATTRIBUTES, IPP_TAG_ENUM);
        write_attr_int(&p, NULL, IPP_OP_GET_JOBS, IPP_TAG_ENUM);
        write_attr_int(&p, NULL, IPP_OP_GET_PRINTER_ATTRIBUTES, IPP_TAG_ENUM);
        
        write_attr_str(&p, "charset-configured", "utf-8", IPP_TAG_CHARSET);
        write_attr_str(&p, "charset-supported", "utf-8", IPP_TAG_CHARSET);
        write_attr_str(&p, "natural-language-configured", "en", IPP_TAG_NATURAL_LANGUAGE);
        write_attr_str(&p, "generated-natural-language-supported", "en", IPP_TAG_NATURAL_LANGUAGE);
        
        write_attr_str(&p, "document-format-default", "image/pwg-raster", IPP_TAG_MIME_MEDIA_TYPE);
        write_attr_str(&p, "document-format-supported", "image/pwg-raster", IPP_TAG_MIME_MEDIA_TYPE);
        write_attr_str(&p, NULL, "application/octet-stream", IPP_TAG_MIME_MEDIA_TYPE);
        write_attr_str(&p, "document-format-preferred", "image/pwg-raster", IPP_TAG_MIME_MEDIA_TYPE);
        write_attr_str(&p, "urf-supported", "V1.4,W8,CP1,RS300-600", IPP_TAG_KEYWORD);
        write_attr_str(&p, "pwg-raster-document-sheet-back", "normal", IPP_TAG_KEYWORD);
        
        write_attr_bool(&p, "printer-is-accepting-jobs", true);
        write_attr_int(&p, "queued-job-count", 0, IPP_TAG_INTEGER);
        write_attr_str(&p, "pdl-override-supported", "attempted", IPP_TAG_KEYWORD);
        write_attr_int(&p, "printer-up-time", (int32_t)(time(NULL) - g_startup_time), IPP_TAG_INTEGER);
        write_attr_str(&p, "compression-supported", "none", IPP_TAG_KEYWORD);
        write_attr_range(&p, "copies-supported", 1, 99);
        write_attr_int(&p, "copies-default", 1, IPP_TAG_INTEGER);
        write_attr_str(&p, "sides-supported", "one-sided", IPP_TAG_KEYWORD);
        write_attr_str(&p, "sides-default", "one-sided", IPP_TAG_KEYWORD);
        write_attr_str(&p, "media-supported", "iso_a4_210x297mm", IPP_TAG_KEYWORD);
        write_attr_str(&p, NULL, "na_letter_8.5x11in", IPP_TAG_KEYWORD);
        write_attr_str(&p, "media-default", "iso_a4_210x297mm", IPP_TAG_KEYWORD);
        write_attr_str(&p, "media-ready", "iso_a4_210x297mm", IPP_TAG_KEYWORD);
        write_attr_str(&p, "media-col-supported", "media-size", IPP_TAG_KEYWORD);
        
        write_attr_resolution(&p, "printer-resolution-default", 600, 600, 3);
        write_attr_resolution(&p, "printer-resolution-supported", 300, 300, 3);
        write_attr_resolution(&p, NULL, 600, 600, 3);
        
        write_attr_int(&p, "orientation-requested-supported", 3, IPP_TAG_ENUM);
        write_attr_int(&p, NULL, 4, IPP_TAG_ENUM);
        write_attr_int(&p, "print-quality-supported", 4, IPP_TAG_ENUM);
        
        write_attr_bool(&p, "color-supported", false);
        write_attr_str(&p, "printer-make-and-model", "Microsoft IPP Class Driver", IPP_TAG_TEXT_WITHOUT_LANGUAGE);
        
        write_attr_str(&p, "pwg-raster-document-type-supported", "sgray_8", IPP_TAG_KEYWORD);
        write_attr_resolution(&p, "pwg-raster-document-resolution-supported", 300, 300, 3);
        write_attr_resolution(&p, NULL, 600, 600, 3);
        
        write_attr_str(&p, "print-color-mode-supported", "monochrome", IPP_TAG_KEYWORD);
        write_attr_str(&p, "print-color-mode-default", "monochrome", IPP_TAG_KEYWORD);
        
    } else if (op_id == IPP_OP_PRINT_JOB) {
        write_u8(&p, IPP_TAG_JOB_ATTRIBUTES);
        write_attr_int(&p, "job-id", 1, IPP_TAG_INTEGER);
        char job_uri[128];
        uint16_t port = config->listen_port ? config->listen_port : 6631;
        snprintf(job_uri, sizeof(job_uri), "ipp://localhost:%u/jobs/1", port);
        write_attr_str(&p, "job-uri", job_uri, IPP_TAG_URI);
        write_attr_int(&p, "job-state", 9, IPP_TAG_ENUM); // 9 = completed
        write_attr_str(&p, "job-state-reasons", "job-completed-successfully", IPP_TAG_KEYWORD);
        
        // Find document data after end-of-attributes tag (0x03)
        size_t offset = 8;
        while (offset < req_len) {
            uint8_t tag = req[offset++];
            if (tag == IPP_TAG_END_OF_ATTRIBUTES) {
                break;
            }
            // Group delimiter tags (0x01-0x05) have no name/value fields
            if (tag <= 0x05) {
                continue;
            }
            // Value tag: skip name-length + name + value-length + value
            if (offset + 2 > req_len) break;
            uint16_t name_len = (req[offset] << 8) | req[offset+1];
            offset += 2 + name_len;
            if (offset + 2 > req_len) break;
            uint16_t val_len = (req[offset] << 8) | req[offset+1];
            offset += 2 + val_len;
        }
        if (offset < req_len) {
            size_t pwg_len = req_len - offset;
            LOG_INFO("Print-Job: extracted %zu bytes of PWG-Raster data", pwg_len);
            if (config->on_print) {
                config->on_print(&req[offset], pwg_len, config->user_data);
            }
        }
        
    } else if (op_id == IPP_OP_VALIDATE_JOB) {
        // Nothing extra needed
    } else if (op_id == IPP_OP_GET_JOB_ATTRIBUTES) {
        write_u8(&p, IPP_TAG_JOB_ATTRIBUTES);
        write_attr_int(&p, "job-id", 1, IPP_TAG_INTEGER);
        write_attr_int(&p, "job-state", 9, IPP_TAG_ENUM); // completed
        write_attr_str(&p, "job-state-reasons", "job-completed-successfully", IPP_TAG_KEYWORD);
    } else if (op_id == IPP_OP_GET_JOBS) {
        // Empty job list
    } else if (op_id == IPP_OP_CANCEL_JOB) {
        // Cancelled
    }

    write_u8(&p, IPP_TAG_END_OF_ATTRIBUTES);
    
    *resp_buf = res;
    *resp_len = p - res;
}

static ssize_t recv_all(socket_t sock, uint8_t *buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(sock, (char*)buf + total, len - total, 0);
        if (r <= 0) return r; // Error or disconnect
        total += r;
    }
    return (ssize_t)total;
}

static int handle_client(socket_t sock, const ipp_server_config_t *config) {
    // Read HTTP headers
    uint8_t *req = malloc(1024 * 1024); // Max 1MB buffer for headers
    if (!req) return -1;
    
    size_t req_len = 0;
    bool headers_done = false;
    int content_length = -1;
    bool chunked = false;
    bool expect_100 = false;
    
    // Read byte by byte for headers
    while (req_len < 1024*1024 - 1) {
        ssize_t r = recv(sock, (char*)&req[req_len], 1, 0);
        if (r <= 0) break;
        req_len++;
        if (req_len >= 4 && memcmp(&req[req_len-4], "\r\n\r\n", 4) == 0) {
            headers_done = true;
            break;
        }
    }
    
    if (!headers_done) {
        free(req);
        return -1;
    }
    
    req[req_len] = '\0';
    
    // Parse request line (e.g., "POST /printers/canon-lbp810 HTTP/1.1")
    char method[16] = {0};
    char path[256] = {0};
    sscanf((char*)req, "%15s %255s", method, path);
    LOG_INFO("HTTP %s %s", method, path);
    
    // Parse headers
    char *h = (char*)req;
    while (h && *h != '\r') {
        char *eol = strstr(h, "\r\n");
        if (!eol) break;
        *eol = '\0';
        
        if (strncasecmp(h, "Content-Length:", 15) == 0) {
            content_length = atoi(h + 15);
        } else if (strncasecmp(h, "Transfer-Encoding:", 18) == 0 && strstr(h, "chunked")) {
            chunked = true;
        } else if (strncasecmp(h, "Expect:", 7) == 0 && strstr(h, "100-continue")) {
            expect_100 = true;
        }
        h = eol + 2;
    }
    
    // Handle GET requests (used by Windows for IPP printer discovery)
    if (strcasecmp(method, "GET") == 0) {
        free(req);
        const char *get_resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 45\r\n"
            "\r\n"
            "<html><body>Canon LBP-810 IPP</body></html>\n";
        send(sock, get_resp, (int)strlen(get_resp), 0);
        return 0;
    }
    
    if (expect_100) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        send(sock, cont, (int)strlen(cont), 0);
    }
    
    // Read body
    uint8_t *body = NULL;
    size_t body_len = 0;
    
    if (chunked) {
        size_t cap = 65536;
        body = malloc(cap);
        while (1) {
            char line[256];
            int line_len = 0;
            while (line_len < 255) {
                ssize_t r = recv(sock, &line[line_len], 1, 0);
                if (r <= 0) break;
                if (line[line_len] == '\n') break;
                line_len++;
            }
            line[line_len] = '\0';
            long chunk_size = strtol(line, NULL, 16);
            if (chunk_size == 0) {
                // Read trailer \r\n
                recv(sock, line, 2, 0);
                break;
            }
            
            if (body_len + chunk_size > cap) {
                cap = (body_len + chunk_size) * 2;
                body = realloc(body, cap);
            }
            if (recv_all(sock, body + body_len, chunk_size) <= 0) break;
            body_len += chunk_size;
            
            // Read \r\n after chunk
            recv(sock, line, 2, 0);
        }
    } else if (content_length > 0) {
        body = malloc(content_length);
        if (recv_all(sock, body, content_length) > 0) {
            body_len = content_length;
        }
    }
    
    free(req);
    
    if (body && body_len > 0) {
        LOG_INFO("Processing IPP request: %zu bytes", body_len);
        uint8_t *resp_buf = NULL;
        size_t resp_len = 0;
        
        handle_ipp_request(body, body_len, &resp_buf, &resp_len, config);
        
        if (resp_buf) {
            char http_header[512];
            snprintf(http_header, sizeof(http_header),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/ipp\r\n"
                     "Connection: close\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n", resp_len);
            send(sock, http_header, (int)strlen(http_header), 0);
            send(sock, (char*)resp_buf, (int)resp_len, 0);
            free(resp_buf);
        }
    } else {
        LOG_WARN("No body in HTTP %s request to %s", method, path);
    }
    
    if (body) free(body);
    return 0;
}

int ipp_server_run(const ipp_server_config_t *config) {
    g_startup_time = (uint32_t)time(NULL);
    g_stop_server = false;
    
    platform_net_init();
    
    socket_t srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv == INVALID_SOCK) {
        LOG_ERROR("Failed to create IPP socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(config->listen_port ? config->listen_port : 6631);
    
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind IPP socket to port %u", ntohs(addr.sin_port));
        close_socket(srv);
        return -1;
    }
    
    if (listen(srv, 5) < 0) {
        LOG_ERROR("Failed to listen on IPP socket");
        close_socket(srv);
        return -1;
    }
    
    LOG_INFO("IPP Server listening on 127.0.0.1:%u", ntohs(addr.sin_port));
    
    while (!(*config->stop_flag) && !g_stop_server) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000; // 500ms
        
        int r = select((int)(srv + 1), &fds, NULL, NULL, &tv);
        if (r < 0) break;
        if (r == 0) continue; // Timeout, check flags
        
        if (FD_ISSET(srv, &fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            socket_t client = accept(srv, (struct sockaddr*)&client_addr, &client_len);
            if (client != INVALID_SOCK) {
                handle_client(client, config);
                close_socket(client);
            }
        }
    }
    
    close_socket(srv);
    platform_net_cleanup();
    LOG_INFO("IPP Server stopped");
    return 0;
}
