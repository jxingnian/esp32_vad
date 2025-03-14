#ifndef __DOUBAO_TTS_MAIN_H__
#define __DOUBAO_TTS_MAIN_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

/* 函数声明 */
esp_err_t doubao_websocket_init(const char *uri, bool is_ssl, const char *appid, const char *token);
esp_err_t doubao_send_tts_request(const char* text, const char* voice_name, float speed);
void doubao_websocket_cleanup(void);

#endif

