#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "ollama_main.h"

static const char *TAG = "OLLAMA";
static char *s_ollama_uri = NULL;
static esp_http_client_handle_t s_client = NULL;

// HTTP事件处理函数
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // 处理接收到的数据
            ESP_LOGI(TAG, "Received data: %.*s", evt->data_len, (char*)evt->data);
            break;
        default:
            break;
    }
    return ESP_OK;
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
} 