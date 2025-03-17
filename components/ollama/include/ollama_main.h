#ifndef OLLAMA_MAIN_H
#define OLLAMA_MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Ollama响应回调函数类型
 * 
 * @param response 从Ollama接收到的响应文本
 */
typedef void (*ollama_response_callback_t)(const char *response);

/**
 * @brief 初始化Ollama客户端
 * 
 * @param ollama_uri Ollama服务器的URI
 * @return esp_err_t 
 */
esp_err_t ollama_init(const char *ollama_uri);

/**
 * @brief 设置Ollama响应回调函数
 * 
 * @param callback 回调函数指针
 */
void ollama_set_response_callback(ollama_response_callback_t callback);

/**
 * @brief 发送文本到Ollama进行对话
 * 
 * @param text 要发送的文本
 * @return esp_err_t 
 */
esp_err_t ollama_chat(const char *text);

/**
 * @brief 清理Ollama客户端资源
 */
void ollama_cleanup(void);

#endif /* OLLAMA_MAIN_H */ 