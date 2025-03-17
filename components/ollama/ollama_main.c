#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "ollama_main.h"

static const char *TAG = "OLLAMA";
static char *s_ollama_uri = NULL;
static esp_http_client_handle_t s_client = NULL;
static ollama_response_callback_t s_response_callback = NULL;

// 用于累积响应文本的缓冲区
static char *s_accumulated_text = NULL;
static size_t s_accumulated_len = 0;

// HTTP事件处理函数
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *response_buffer = NULL;
    static int response_len = 0;

    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // 处理接收到的数据
            ESP_LOGI(TAG, "Received data: %.*s", evt->data_len, (char*)evt->data);
            
            // 解析JSON响应
            cJSON *root = cJSON_Parse(evt->data);
            if (root) {
                // 检查是否有响应文本
                cJSON *response = cJSON_GetObjectItem(root, "response");
                if (response && cJSON_IsString(response)) {
                    // 累积响应文本
                    const char *text = response->valuestring;
                    if (text && strlen(text) > 0) {
                        // 检查是否为问号，如果是则跳过
                        if (strcmp(text, "？") == 0) {
                            ESP_LOGI(TAG, "跳过问号");
                            cJSON_Delete(root);
                            break;
                        }
                        
                        if (s_accumulated_text == NULL) {
                            s_accumulated_text = strdup(text);
                            if (s_accumulated_text) {
                                s_accumulated_len = strlen(text);
                            }
                        } else {
                            size_t new_len = s_accumulated_len + strlen(text);
                            char *new_buffer = realloc(s_accumulated_text, new_len + 1);
                            if (new_buffer) {
                                s_accumulated_text = new_buffer;
                                strcat(s_accumulated_text, text);
                                s_accumulated_len = new_len;
                            }
                        }
                    }
                }
                
                // 检查是否完成
                cJSON *done = cJSON_GetObjectItem(root, "done");
                if (done && cJSON_IsTrue(done) && s_response_callback && s_accumulated_text) {
                    // 处理累积的文本
                    ESP_LOGI(TAG, "处理累积文本: %s", s_accumulated_text);
                    s_response_callback(s_accumulated_text);
                    
                    // 清理累积的文本
                    free(s_accumulated_text);
                    s_accumulated_text = NULL;
                    s_accumulated_len = 0;
                }
                
                cJSON_Delete(root);
            } else {
                // 如果不是完整的JSON，则累积数据
                if (response_buffer == NULL) {
                    response_buffer = malloc(evt->data_len + 1);
                    if (response_buffer) {
                        memcpy(response_buffer, evt->data, evt->data_len);
                        response_len = evt->data_len;
                        response_buffer[response_len] = 0;
                    }
                } else {
                    char *new_buffer = realloc(response_buffer, response_len + evt->data_len + 1);
                    if (new_buffer) {
                        response_buffer = new_buffer;
                        memcpy(response_buffer + response_len, evt->data, evt->data_len);
                        response_len += evt->data_len;
                        response_buffer[response_len] = 0;
                    }
                }
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            // 请求完成，处理累积的数据
            if (response_buffer) {
                cJSON *root = cJSON_Parse(response_buffer);
                if (root) {
                    // 检查是否有响应文本
                    cJSON *response = cJSON_GetObjectItem(root, "response");
                    if (response && cJSON_IsString(response)) {
                        const char *text = response->valuestring;
                        if (text && strlen(text) > 0) {
                            if (s_accumulated_text == NULL) {
                                s_accumulated_text = strdup(text);
                                if (s_accumulated_text) {
                                    s_accumulated_len = strlen(text);
                                }
                            } else {
                                size_t new_len = s_accumulated_len + strlen(text);
                                char *new_buffer = realloc(s_accumulated_text, new_len + 1);
                                if (new_buffer) {
                                    s_accumulated_text = new_buffer;
                                    strcat(s_accumulated_text, text);
                                    s_accumulated_len = new_len;
                                }
                            }
                        }
                    }
                    
                    // 检查是否完成
                    cJSON *done = cJSON_GetObjectItem(root, "done");
                    if (done && cJSON_IsTrue(done) && s_response_callback && s_accumulated_text) {
                        // 处理累积的文本
                        ESP_LOGI(TAG, "处理累积文本: %s", s_accumulated_text);
                        s_response_callback(s_accumulated_text);
                        
                        // 清理累积的文本
                        free(s_accumulated_text);
                        s_accumulated_text = NULL;
                        s_accumulated_len = 0;
                    }
                    
                    cJSON_Delete(root);
                }
                free(response_buffer);
                response_buffer = NULL;
                response_len = 0;
            }
            break;
            
        case HTTP_EVENT_DISCONNECTED:
            // 连接断开，清理资源
            if (response_buffer) {
                free(response_buffer);
                response_buffer = NULL;
                response_len = 0;
            }
            
            // 如果连接断开但还有累积的文本，也触发回调
            if (s_accumulated_text && s_response_callback) {
                ESP_LOGI(TAG, "连接断开，处理累积文本: %s", s_accumulated_text);
                s_response_callback(s_accumulated_text);
                free(s_accumulated_text);
                s_accumulated_text = NULL;
                s_accumulated_len = 0;
            }
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

void ollama_set_response_callback(ollama_response_callback_t callback)
{
    s_response_callback = callback;
}

esp_err_t ollama_init(const char *ollama_uri)
{
    if (s_ollama_uri) {
        free(s_ollama_uri);
    }
    
    s_ollama_uri = strdup(ollama_uri);
    if (!s_ollama_uri) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = s_ollama_uri,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };
    
    s_client = esp_http_client_init(&config);
    if (!s_client) {
        free(s_ollama_uri);
        s_ollama_uri = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ollama_chat(const char *text)
{
    if (!s_client || !text) {
        return ESP_ERR_INVALID_STATE;
    }

    // 清理之前可能存在的累积文本
    if (s_accumulated_text) {
        free(s_accumulated_text);
        s_accumulated_text = NULL;
        s_accumulated_len = 0;
    }

    // 构建JSON请求体
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "qwen2:0.5b");
    cJSON_AddStringToObject(root, "prompt", text);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!post_data) {
        return ESP_ERR_NO_MEM;
    }

    // 设置HTTP请求参数
    esp_http_client_set_url(s_client, s_ollama_uri);
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    esp_http_client_set_post_field(s_client, post_data, strlen(post_data));

    // 发送请求
    esp_err_t err = esp_http_client_perform(s_client);
    free(post_data);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(s_client);
        ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);
    } else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    return err;
}

void ollama_cleanup(void)
{
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = NULL;
    }
    
    if (s_ollama_uri) {
        free(s_ollama_uri);
        s_ollama_uri = NULL;
    }
    
    if (s_accumulated_text) {
        free(s_accumulated_text);
        s_accumulated_text = NULL;
        s_accumulated_len = 0;
    }
    
    s_response_callback = NULL;
} 