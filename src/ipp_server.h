#ifndef IPP_SERVER_H
#define IPP_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ipp_print_callback_t)(const uint8_t *pwg_data, size_t pwg_len, void *user_data);

typedef struct {
    uint16_t listen_port;          // Default: 6631
    const char *printer_name;       // "Canon LBP-810"
    ipp_print_callback_t on_print;  // Called when Print-Job received
    void *user_data;                // Passed to callback
    volatile bool *stop_flag;       // Set to true to stop server
} ipp_server_config_t;

int ipp_server_run(const ipp_server_config_t *config);  // Blocks, runs event loop
void ipp_server_stop(void);                              // Signal stop

#ifdef __cplusplus
}
#endif

#endif // IPP_SERVER_H
