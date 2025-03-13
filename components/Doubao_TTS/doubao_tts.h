#ifndef DOUBAO_TTS_H
#define DOUBAO_TTS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief 初始化并连接豆包TTS WebSocket客户端
 * 
 * @param uri WebSocket服务器地址
 * @param is_ssl 是否使用SSL连接
 * @param appid 应用ID
 * @param token 访问令牌
 * @return esp_err_t ESP_OK:成功 ESP_FAIL:失败
 */
esp_err_t doubao_websocket_init(const char *uri, bool is_ssl, const char *appid, const char *token);

/**
 * @brief 发送豆包TTS请求
 * 
 * @param text 要合成的文本
 * @param voice_name 音色代号
 * @param speed 语速(0.5-2.0)
 * @return esp_err_t ESP_OK:成功 ESP_FAIL:失败
 */
esp_err_t doubao_send_tts_request(const char* text, const char* voice_name, float speed);

/**
 * @brief 发送豆包TTS结束帧
 * 
 * @return esp_err_t ESP_OK:成功 ESP_FAIL:失败
 */
esp_err_t doubao_send_finish_frame(void);

/**
 * @brief 清理豆包TTS WebSocket连接
 */
void doubao_websocket_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* DOUBAO_TTS_H */ 