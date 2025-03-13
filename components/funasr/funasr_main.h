/*
 * @Author: xingnian j_xingnian@163.com
 * @Date: 2025-03-10 13:39:52
 * @LastEditors: xingnian j_xingnian@163.com
 * @LastEditTime: 2025-03-13 17:40:19
 * @FilePath: \esp32_vad\components\funasr\funasr_main.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef __FUNASR_MAIN_H__
#define __FUNASR_MAIN_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

/* 函数声明 */
esp_err_t funasr_websocket_init(const char *uri, bool is_ssl);
esp_err_t funasr_send_start_frame(void);
esp_err_t funasr_send_finish_frame(void);
esp_err_t funasr_websocket_send_audio(const uint8_t *data, size_t len);
void funasr_websocket_cleanup(void);

#endif

