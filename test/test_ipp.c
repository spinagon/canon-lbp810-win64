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

static void on_print(const uint8_t *pwg_data, size_t pwg_len, void *user_data) {
    (void)user_data;
    g_print_job_count++;
    g_last_pwg_len = pwg_len;
}

static void *server_thread(void *arg) {
    (void)arg;
    ipp_server_config_t config = {
        .listen_port = 6310, // Test port
        .printer_name = "Canon LBP-810 Test",
        .on_print = on_print,
        .user_data = NULL,
        .stop_flag = &g_stop_flag
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

    char resp[8192];
    int len = recv(sock, resp, sizeof(resp) - 1, 0);
    assert(len > 0);
    resp[len] = '\0';

    // Verify it responds with 200 OK and valid IPP
    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "application/ipp") != NULL);

    close_socket(sock);
    printf("Test Get-Printer-Attributes passed.\n");
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
    test_print_job();
    test_chunked_transfer();

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
