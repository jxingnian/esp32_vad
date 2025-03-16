/*
 * 豆包TTS WebSocket客户端
 * 
 * 该模块实现了与豆包TTS服务的WebSocket通信功能,包括:
 * - WebSocket连接的建立和管理
 * - TTS请求的发送
 * - 音频数据的接收和处理
 * 
 * 使用方法:
 * 1. 调用 doubao_tts_init() 初始化连接
 * 2. 调用 doubao_tts_request() 发送TTS请求
 * 3. 在WebSocket事件处理函数中接收音频数据
 * 4. 使用完毕后调用 doubao_tts_cleanup() 清理资源
 * 
 * 配置要求:
 * 1. 在sdkconfig中设置 DOUBAO_APPID 和 DOUBAO_TOKEN
 * 2. 确保设备已连接网络
 * 
 * 参考文档:
 * - 豆包TTS API: https://www.volcengine.com/docs/6561/1257584
 * - ESP WebSocket: https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/
 * 
 * 作者: 星年
 * 日期: 2025-03-20
 */

#ifndef __DOUBAO_MAIN_H__
#define __DOUBAO_MAIN_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#define CONFIG_DOUBAO_APPID "2013524180"
#define CONFIG_DOUBAO_TOKEN "32jGMq9t0hZz9nZpzKt1ZwLro-aTvY8W"

/* 音频数据回调函数类型 */
typedef void (*doubao_audio_callback_t)(const uint8_t *audio_data, size_t len);

/**
 * @brief 初始化WebSocket连接
 * 
 * @param uri WebSocket服务器地址(wss://openspeech.bytedance.com/api/v1/tts)
 * @return esp_err_t ESP_OK:成功 其他:失败
 */
esp_err_t doubao_tts_init(const char *uri);

/**
 * @brief 发送TTS合成请求
 * 
 * @param text 要合成的文本
 * @param voice_type 音色类型(如:"zh_female_voice")
 * @return esp_err_t ESP_OK:成功 其他:失败
 */
esp_err_t doubao_tts_request(const char *text, const char *voice_type);

/**
 * @brief 清理WebSocket资源
 */
void doubao_tts_cleanup(void);

/**
 * @brief 设置音频数据回调函数
 * 
 * @param callback 回调函数指针
 */
void doubao_tts_set_audio_callback(doubao_audio_callback_t callback);

#endif /* __DOUBAO_MAIN_H__ */

