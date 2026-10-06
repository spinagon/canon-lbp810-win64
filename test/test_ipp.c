#include "../src/ipp_server.h"
#include "../src/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#ifndef _WIN32
#include <pthread.h>
#endif

static volatile bool g_stop_flag = false;
static int g_print_job_count = 0;
static size_t g_last_pwg_len = 0;
static bool   g_cancel_called = false;
static ipp_printer_state_info_t g_mock_status = {
    .printer_state = 3,
    .state_reasons = "none",
    .is_accepting_jobs = true
};

static void (*g_active_on_print)(const uint8_t *, size_t, void *) = NULL;

static void on_print(const uint8_t *pwg_data, size_t pwg_len, void *user_data) {
    if (g_active_on_print) {
        g_active_on_print(pwg_data, pwg_len, user_data);
        return;
    }
    (void)user_data;
    g_print_job_count++;
    g_last_pwg_len = pwg_len;
}

static void on_cancel(void *user_data) {
    (void)user_data;
    g_cancel_called = true;
}

static void get_status(ipp_printer_state_info_t *info, void *user_data) {
    (void)user_data;
    *info = g_mock_status;
}

static void *server_thread(void *arg) {
    (void)arg;
    ipp_server_config_t config = {
        .listen_port  = 6310, // Test port
        .printer_name = "Canon LBP-810 Test",
        .on_print     = on_print,
        .on_cancel    = on_cancel,
        .get_status   = get_status,
        .user_data    = NULL,
        .stop_flag    = &g_stop_flag
    };
    ipp_server_run(&config);
    return NULL;
}

#ifdef _WIN32
static DWORD WINAPI win32_server_thread(LPVOID arg) {
    server_thread(arg);
    return 0;
}
#endif

static socket_t connect_server(void) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCK) return INVALID_SOCK;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6310);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    // Wait for server to start
    for (int i = 0; i < 20; i++) {
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) return sock;
        platform_sleep_ms(100);
    }
    close_socket(sock);
    return INVALID_SOCK;
}

static bool contains_bytes(const char *buf, size_t buf_len, const char *pattern) {
    size_t pat_len = strlen(pattern);
    if (pat_len > buf_len) return false;
    for (size_t i = 0; i <= buf_len - pat_len; i++) {
        if (memcmp(buf + i, pattern, pat_len) == 0) return true;
    }
    return false;
}

static void test_get_printer_attributes(void) {
    socket_t sock = connect_server();
    assert(sock != INVALID_SOCK);

    // IPP Get-Printer-Attributes request
    uint8_t ipp_req[] = {
        0x02, 0x00, // Version 2.0
        0x00, 0x0B, // Get-Printer-Attributes
        0x00, 0x00, 0x00, 0x01, // Request ID 1
        0x01, // Operation attributes
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03 // End of attributes
    };

    char http_req[1024];
    snprintf(http_req, sizeof(http_req),
             "POST /printers/canon-lbp810 HTTP/1.1\r\n"
             "Content-Length: %zu\r\n\r\n", sizeof(ipp_req));

    send(sock, http_req, (int)strlen(http_req), 0);
    send(sock, (const char*)ipp_req, (int)sizeof(ipp_req), 0);

    char resp[16384];
    int len = 0;
    while (len < (int)sizeof(resp) - 1) {
        int r = recv(sock, resp + len, (int)sizeof(resp) - 1 - len, 0);
        if (r <= 0) break;
        len += r;
    }
    assert(len > 0);
    resp[len] = '\0';

    // Verify it responds with 200 OK and valid IPP
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "application/ipp") != NULL);

    // Verify extended media formats are advertised (Roadmap 2.1)
    assert(contains_bytes(resp, len, "iso_a4_210x297mm"));
    assert(contains_bytes(resp, len, "na_letter_8.5x11in"));
    assert(contains_bytes(resp, len, "na_legal_8.5x14in"));
    assert(contains_bytes(resp, len, "na_executive_7.25x10.5in"));
    assert(contains_bytes(resp, len, "iso_a5_148x210mm"));
    assert(contains_bytes(resp, len, "jis_b5_182x257mm"));
    assert(contains_bytes(resp, len, "na_number-10_4.125x9.5in"));
    assert(contains_bytes(resp, len, "iso_dl_110x220mm"));
    assert(contains_bytes(resp, len, "iso_c5_162x229mm"));

    // Verify media source and quality (Roadmap 2.2 & 2.3)
    assert(contains_bytes(resp, len, "media-source-supported"));
    assert(contains_bytes(resp, len, "print-quality-supported"));

    close_socket(sock);
    printf("Test Get-Printer-Attributes passed.\n");
}

static void test_dynamic_status(void) {
    g_mock_status.printer_state = 5; // stopped
    g_mock_status.state_reasons = "door-open-error";
    g_mock_status.is_accepting_jobs = false;

    socket_t sock = connect_server();
    assert(sock != INVALID_SOCK);

    uint8_t ipp_req[] = {
        0x02, 0x00,
        0x00, 0x0B, // Get-Printer-Attributes
        0x00, 0x00, 0x00, 0x06,
        0x01,
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03
    };

    char http_req[1024];
    snprintf(http_req, sizeof(http_req),
             "POST /printers/canon-lbp810 HTTP/1.1\r\n"
             "Content-Length: %zu\r\n\r\n", sizeof(ipp_req));

    send(sock, http_req, (int)strlen(http_req), 0);
    send(sock, (const char*)ipp_req, (int)sizeof(ipp_req), 0);

    char resp[16384];
    int len = 0;
    while (len < (int)sizeof(resp) - 1) {
        int r = recv(sock, resp + len, (int)sizeof(resp) - 1 - len, 0);
        if (r <= 0) break;
        len += r;
    }
    assert(len > 0);
    resp[len] = '\0';
    close_socket(sock);

    assert(contains_bytes(resp, len, "door-open-error"));

    // Reset status back to idle
    g_mock_status.printer_state = 3;
    g_mock_status.state_reasons = "none";
    g_mock_status.is_accepting_jobs = true;

    printf("Test dynamic status reporting passed.\n");
}

static void test_cancel_job(void) {
    socket_t sock = connect_server();
    assert(sock != INVALID_SOCK);

    g_cancel_called = false;

    uint8_t ipp_req[] = {
        0x02, 0x00,
        0x00, 0x08, // Cancel-Job
        0x00, 0x00, 0x00, 0x07,
        0x01,
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03
    };

    char http_req[1024];
    snprintf(http_req, sizeof(http_req),
             "POST /printers/canon-lbp810 HTTP/1.1\r\n"
             "Content-Length: %zu\r\n\r\n", sizeof(ipp_req));

    send(sock, http_req, (int)strlen(http_req), 0);
    send(sock, (const char*)ipp_req, (int)sizeof(ipp_req), 0);

    char resp[4096];
    int len = 0;
    while (len < (int)sizeof(resp) - 1) {
        int r = recv(sock, resp + len, (int)sizeof(resp) - 1 - len, 0);
        if (r <= 0) break;
        len += r;
    }
    assert(len > 0);
    resp[len] = '\0';
    close_socket(sock);

    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(contains_bytes(resp, len, "job-canceled-by-user"));
    assert(g_cancel_called == true);

    printf("Test Cancel-Job passed.\n");
}

static void test_print_job(void) {
    socket_t sock = connect_server();
    assert(sock != INVALID_SOCK);

    uint8_t ipp_req[] = {
        0x02, 0x00,
        0x00, 0x02, // Print-Job
        0x00, 0x00, 0x00, 0x02, // Request ID 2
        0x01, // Operation attributes
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03, // End of attributes
        'P', 'W', 'G', 'R', 'A', 'S', 'T', 'E', 'R' // Dummy document data (9 bytes)
    };

    char http_req[1024];
    snprintf(http_req, sizeof(http_req),
             "POST /printers/canon-lbp810 HTTP/1.1\r\n"
             "Content-Length: %zu\r\n\r\n", sizeof(ipp_req));

    send(sock, http_req, (int)strlen(http_req), 0);
    send(sock, (const char*)ipp_req, (int)sizeof(ipp_req), 0);

    char resp[1024];
    int len = recv(sock, resp, sizeof(resp), 0);
    assert(len > 0);
    close_socket(sock);

    // Wait for callback (poll up to 2 seconds)
    for (int i = 0; i < 200; i++) {
        platform_sleep_ms(10);
        if (g_print_job_count >= 1) break;
    }
    assert(g_print_job_count == 1);
    assert(g_last_pwg_len == 9);

    printf("Test Print-Job passed.\n");
}

static void test_chunked_transfer(void) {
    socket_t sock = connect_server();
    assert(sock != INVALID_SOCK);

    char http_req[] = 
        "POST /printers/canon-lbp810 HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    send(sock, http_req, (int)strlen(http_req), 0);

    // Send in chunks
    uint8_t chunk1[] = {
        0x02, 0x00,
        0x00, 0x04, // Validate-Job
        0x00, 0x00, 0x00, 0x03,
        0x01
    };
    uint8_t chunk2[] = {
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03
    };

    char chunk_hdr[32];
    snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", (unsigned)sizeof(chunk1));
    send(sock, chunk_hdr, (int)strlen(chunk_hdr), 0);
    send(sock, (const char*)chunk1, (int)sizeof(chunk1), 0);
    send(sock, "\r\n", 2, 0);

    snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", (unsigned)sizeof(chunk2));
    send(sock, chunk_hdr, (int)strlen(chunk_hdr), 0);
    send(sock, (const char*)chunk2, (int)sizeof(chunk2), 0);
    send(sock, "\r\n", 2, 0);

    send(sock, "0\r\n\r\n", 5, 0);

    char resp[1024];
    int len = recv(sock, resp, sizeof(resp), 0);
    assert(len > 0);
    close_socket(sock);

    printf("Test chunked transfer passed.\n");
}

static volatile bool g_concurrent_print_started = false;
static volatile bool g_concurrent_print_detected_cancel = false;

static void on_concurrent_print(const uint8_t *pwg_data, size_t pwg_len, void *user_data) {
    (void)pwg_data; (void)pwg_len; (void)user_data;
    g_concurrent_print_started = true;
    for (int i = 0; i < 50; i++) {
        if (g_cancel_called) {
            g_concurrent_print_detected_cancel = true;
            break;
        }
        platform_sleep_ms(20);
    }
}

typedef struct {
    socket_t sock;
    uint8_t *req;
    size_t   req_len;
} print_sender_arg_t;

static void *print_sender_thread(void *arg) {
    print_sender_arg_t *p = (print_sender_arg_t *)arg;
    char http_req[1024];
    snprintf(http_req, sizeof(http_req),
             "POST /printers/canon-lbp810 HTTP/1.1\r\n"
             "Content-Length: %zu\r\n\r\n", p->req_len);
    send(p->sock, http_req, (int)strlen(http_req), 0);
    send(p->sock, (const char *)p->req, (int)p->req_len, 0);

    char resp[1024];
    recv(p->sock, resp, sizeof(resp), 0);
    return NULL;
}

static void test_concurrent_cancel_during_print(void) {
    g_cancel_called = false;
    g_concurrent_print_started = false;
    g_concurrent_print_detected_cancel = false;
    g_active_on_print = on_concurrent_print;

    socket_t print_sock = connect_server();
    assert(print_sock != INVALID_SOCK);

    uint8_t ipp_req[] = {
        0x02, 0x00,
        0x00, 0x02, // Print-Job
        0x00, 0x00, 0x00, 0x05, // Request ID 5
        0x01, // Operation attributes
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03, // End of attributes
        'P', 'W', 'G', 'R', 'A', 'S', 'T', 'E', 'R'
    };

    print_sender_arg_t sender_arg = {
        .sock = print_sock,
        .req = ipp_req,
        .req_len = sizeof(ipp_req)
    };

#ifdef _WIN32
    HANDLE send_th = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)print_sender_thread, &sender_arg, 0, NULL);
    assert(send_th != NULL);
#else
    pthread_t send_th;
    int prc = pthread_create(&send_th, NULL, print_sender_thread, &sender_arg);
    assert(prc == 0);
#endif

    // Wait until the print job starts and is actively printing
    for (int i = 0; i < 100; i++) {
        if (g_concurrent_print_started) break;
        platform_sleep_ms(10);
    }
    assert(g_concurrent_print_started == true);

    // Now, while the print job is STILL running in the background, send Cancel-Job on a new connection
    socket_t cancel_sock = connect_server();
    assert(cancel_sock != INVALID_SOCK);

    uint8_t cancel_req[] = {
        0x02, 0x00,
        0x00, 0x08, // Cancel-Job
        0x00, 0x00, 0x00, 0x06,
        0x01,
        0x47, 0x00, 0x12, 'a', 't', 't', 'r', 'i', 'b', 'u', 't', 'e', 's', '-', 'c', 'h', 'a', 'r', 's', 'e', 't', 0x00, 0x05, 'u', 't', 'f', '-', '8',
        0x03
    };

    char cancel_http[1024];
    snprintf(cancel_http, sizeof(cancel_http),
             "POST /printers/canon-lbp810 HTTP/1.1\r\n"
             "Content-Length: %zu\r\n\r\n", sizeof(cancel_req));
    send(cancel_sock, cancel_http, (int)strlen(cancel_http), 0);
    send(cancel_sock, (const char *)cancel_req, (int)sizeof(cancel_req), 0);

    char cancel_resp[4096];
    int rlen = 0;
    while (rlen < (int)sizeof(cancel_resp) - 1) {
        int r = recv(cancel_sock, cancel_resp + rlen, (int)sizeof(cancel_resp) - 1 - rlen, 0);
        if (r <= 0) break;
        rlen += r;
    }
    assert(rlen > 0);
    cancel_resp[rlen] = '\0';
    close_socket(cancel_sock);

    assert(strstr(cancel_resp, "HTTP/1.1 200 OK") != NULL);
    assert(contains_bytes(cancel_resp, rlen, "job-canceled-by-user"));

#ifdef _WIN32
    WaitForSingleObject(send_th, INFINITE);
    CloseHandle(send_th);
#else
    pthread_join(send_th, NULL);
#endif
    close_socket(print_sock);

    assert(g_cancel_called == true);
    assert(g_concurrent_print_detected_cancel == true);

    g_active_on_print = NULL;
    printf("Test concurrent Cancel-Job during active print passed.\n");
}

int main(void) {
    printf("Running IPP tests...\n");
    platform_net_init();

#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, win32_server_thread, NULL, 0, NULL);
    assert(thread != NULL);
#else
    pthread_t thread;
    pthread_create(&thread, NULL, server_thread, NULL);
#endif

    test_get_printer_attributes();
    test_dynamic_status();
    test_cancel_job();
    test_print_job();
    test_chunked_transfer();
    test_concurrent_cancel_during_print();

    g_stop_flag = true;
    ipp_server_stop();

#ifdef _WIN32
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_join(thread, NULL);
#endif

    platform_net_cleanup();
    printf("All tests passed.\n");
    return 0;
}
