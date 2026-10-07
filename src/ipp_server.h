#ifndef IPP_SERVER_H
#define IPP_SERVER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ipp_print_callback_t)(const uint8_t *pwg_data, size_t pwg_len, void *user_data);
typedef void (*ipp_cancel_callback_t)(void *user_data);

typedef struct {
    int         printer_state;     /* 3 = idle, 4 = processing, 5 = stopped */
    const char *state_reasons;     /* e.g. "none", "media-empty-error", "door-open-error", "media-jam-error", "marker-supply-missing-error" */
    bool        is_accepting_jobs;
} ipp_printer_state_info_t;

typedef void (*ipp_get_status_callback_t)(ipp_printer_state_info_t *info, void *user_data);

typedef struct {
    uint16_t                  listen_port;   // Default: 6631
    const char               *printer_name;  // "Canon LBP-810"
    ipp_print_callback_t      on_print;      // Called when Print-Job received
    ipp_cancel_callback_t     on_cancel;     // Called when Cancel-Job received
    ipp_get_status_callback_t get_status;    // Called to query printer state
    void                     *user_data;     // Passed to callback
    volatile bool            *stop_flag;     // Set to true to stop server
} ipp_server_config_t;

int ipp_server_run(const ipp_server_config_t *config);  // Blocks, runs event loop
void ipp_server_stop(void);                              // Signal stop

#ifdef __cplusplus
}
#endif

#endif // IPP_SERVER_H
