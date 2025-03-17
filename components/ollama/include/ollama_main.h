#ifndef OLLAMA_MAIN_H
#define OLLAMA_MAIN_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief 初始化Ollama客户端
 * 
 * @param ollama_uri Ollama服务器的URI
 * @return esp_err_t 
 */
esp_err_t ollama_init(const char *ollama_uri);

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