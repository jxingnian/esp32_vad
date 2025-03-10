#ifndef __FUNASR_MAIN_H__
#define __FUNASR_MAIN_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

/* 函数声明 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
esp_err_t websocket_init(const char *uri, bool is_ssl);
esp_err_t send_start_frame(void);
esp_err_t send_finish_frame(void);
esp_err_t websocket_send_audio(const uint8_t *data, size_t len);
void websocket_cleanup(void);

#endif

