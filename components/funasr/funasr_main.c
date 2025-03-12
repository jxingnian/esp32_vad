/*
 * FunASR WebSocket Client
 * 
 * 该模块实现了与FunASR服务器的WebSocket通信功能,包括:
 * - WebSocket连接的建立和管理
 * - 音频数据的发送
 * - 识别结果的接收和处理
 * 
 * 作者: 星年
 * 日期: 2024-01-20
 */

#include "funasr_main.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"

/* 日志标签 */
static const char *TAG = "FUNASR_WEBSOCKET";

/* WebSocket客户端句柄 */
esp_websocket_client_handle_t client;

/* 保存WebSocket连接参数的全局变量 */
static struct {
    char uri[128];
    bool is_ssl;
} ws_config = {0};

/* 函数声明 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
esp_err_t websocket_init(const char *uri, bool is_ssl);
esp_err_t send_start_frame(void);
esp_err_t send_finish_frame(void);
esp_err_t websocket_send_audio(const uint8_t *data, size_t len);
void websocket_cleanup(void);

/* WebSocket事件处理函数
 * 该函数处理所有WebSocket相关事件,包括连接、断开连接、数据接收和错误
 * 
 * 参数:
 *   handler_args: 事件处理器的参数
 *   base: 事件基类型
 *   event_id: 事件ID,用于区分不同类型的事件
 *   event_data: 事件相关的数据
 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    /* 将事件数据转换为WebSocket事件数据结构 */
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    /* 根据事件ID进行不同的处理 */
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            /* WebSocket连接建立成功 */
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            /* WebSocket连接断开 */
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_DISCONNECTED: 连接断开");
            ESP_LOGI(TAG, "正在尝试重新连接...");
            
            /* 停止当前客户端 */
            esp_err_t stop_err = esp_websocket_client_stop(client);
            if (stop_err != ESP_OK) {
                ESP_LOGE(TAG, "停止WebSocket客户端失败: %s", esp_err_to_name(stop_err));
            }
            
            /* 使用保存的配置重新初始化并启动客户端 */
            esp_websocket_client_config_t websocket_cfg = {
                .uri = ws_config.uri,
                .disable_auto_reconnect = false,
                .task_stack = 4096,
                .task_prio = 5,
                .buffer_size = 1024,
                .transport = ws_config.is_ssl ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP,
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            
            /* 重新初始化客户端 */
            client = esp_websocket_client_init(&websocket_cfg);
            if (client == NULL) {
                ESP_LOGE(TAG, "重新初始化WebSocket客户端失败");
                break;
            }
            
            /* 重新注册事件处理函数 */
            esp_err_t reg_err = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
            if (reg_err != ESP_OK) {
                ESP_LOGE(TAG, "注册事件处理函数失败: %s", esp_err_to_name(reg_err));
                break;
            }
            
            /* 启动客户端 */
            esp_err_t start_err = esp_websocket_client_start(client);
            if (start_err != ESP_OK) {
                ESP_LOGE(TAG, "启动WebSocket客户端失败: %s", esp_err_to_name(start_err));
            }
            
            break;
        case WEBSOCKET_EVENT_DATA:
            /* 接收到WebSocket数据 */
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");
            ESP_LOGI(TAG, "Received opcode=%d", data->op_code);
            
            /* 检查是否为关闭帧(opcode 0x08)或心跳帧(opcode 0x0A) */
            if (data->op_code == 0x08 && data->data_len == 2) {
                /* 解析关闭状态码(由两个字节组成) */
                ESP_LOGW(TAG, "收到关闭消息,状态码=%d", 256*data->data_ptr[0] + data->data_ptr[1]);
            } else if (data->op_code == 0x0A) {
                /* 收到心跳帧,忽略处理 */
                ESP_LOGI(TAG, "收到心跳帧");
            } else if (data->data_ptr == NULL) {
                ESP_LOGE(TAG, "接收到空数据");
            } else {
                /* 解析接收到的JSON数据 */
                cJSON *root = cJSON_Parse((char *)data->data_ptr);
                if (root == NULL) {
                    ESP_LOGE(TAG, "JSON解析失败");
                    break;
                }

                /* 获取各个字段 */
                const char *mode = cJSON_GetObjectItem(root, "mode")->valuestring;
                const char *wav_name = cJSON_GetObjectItem(root, "wav_name")->valuestring;
                const char *text = cJSON_GetObjectItem(root, "text")->valuestring;
                bool is_final = cJSON_GetObjectItem(root, "is_final")->valueint;
                
                /* 打印基本信息 */
                if (strcmp(mode, "2pass-offline") == 0) {
                    ESP_LOGI(TAG, "识别文本: %s", text);
                }
                /* 处理时间戳信息(如果存在) */
                cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
                if (timestamp != NULL) {
                    ESP_LOGI(TAG, "时间戳: %s", timestamp->valuestring);
                }

                /* 处理句子级别时间戳(如果存在) */
                cJSON *stamp_sents = cJSON_GetObjectItem(root, "stamp_sents");
                if (stamp_sents != NULL && cJSON_IsArray(stamp_sents)) {
                    int array_size = cJSON_GetArraySize(stamp_sents);
                    for (int i = 0; i < array_size; i++) {
                        cJSON *sent = cJSON_GetArrayItem(stamp_sents, i);
                        const char *text_seg = cJSON_GetObjectItem(sent, "text_seg")->valuestring;
                        const char *punc = cJSON_GetObjectItem(sent, "punc")->valuestring;
                        int start = cJSON_GetObjectItem(sent, "start")->valueint;
                        int end = cJSON_GetObjectItem(sent, "end")->valueint;
                        
                        ESP_LOGI(TAG, "句子[%d]: 文本=%s, 标点=%s, 开始=%d, 结束=%d", 
                                i, text_seg, punc, start, end);
                    }
                }

                /* 释放JSON对象 */
                cJSON_Delete(root);
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            /* WebSocket发生错误 */
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR: WebSocket连接发生错误");
            if (data->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGE(TAG, "传输层错误, 错误码: %d", data->error_handle.esp_tls_last_esp_err);
                ESP_LOGE(TAG, "报告的错误: %s", esp_err_to_name(data->error_handle.esp_tls_last_esp_err));
            }
            break;
    }
}

/**
 * @brief 初始化并连接WebSocket客户端
 * 
 * 该函数完成以下工作:
 * 1. 保存连接参数
 * 2. 配置WebSocket客户端参数
 * 3. 初始化WebSocket客户端
 * 4. 注册事件处理函数
 * 5. 启动WebSocket客户端
 * 
 * @return esp_err_t ESP_OK:成功 ESP_FAIL:失败
 */
esp_err_t websocket_init(const char *uri , bool is_ssl)
{    
    /* 保存连接参数供后续使用 */
    strncpy(ws_config.uri, uri, sizeof(ws_config.uri) - 1);
    ws_config.uri[sizeof(ws_config.uri) - 1] = '\0';
    ws_config.is_ssl = is_ssl;

    /* 配置WebSocket客户端参数 */
    esp_websocket_client_config_t websocket_cfg = {
        .uri = ws_config.uri,
        .disable_auto_reconnect = false,     /* 启用自动重连 */
        .task_stack = 4096,                  /* WebSocket任务栈大小(字节) */
        .task_prio = 5,                      /* WebSocket任务优先级(0-25,数字越大优先级越高) */
        .buffer_size = 1024,                 /* 收发数据缓冲区大小(字节) */
        .transport = ws_config.is_ssl ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP,  /* 使用TCP传输,禁用SSL */
        .crt_bundle_attach = esp_crt_bundle_attach,  /* 附加证书包,用于SSL/TLS连接的证书验证。esp_crt_bundle_attach是ESP-IDF提供的默认证书包 */
    };

    /* 使用配置初始化WebSocket客户端 */
    client = esp_websocket_client_init(&websocket_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return ESP_FAIL;
    }

    /* 注册事件处理函数,处理所有WebSocket事件 */
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    /* 启动WebSocket客户端,开始连接服务器 */
    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client");
        return ret;
    }

    return ESP_OK;
}

/* 发送开始帧 */
esp_err_t send_start_frame() {
    if (client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *data = cJSON_CreateObject();
    /* chunk_interval: 设置音频分片间隔为10帧 */
    cJSON_AddNumberToObject(data, "chunk_interval", 10);
    // cJSON_AddItemToObject(data, "chunk_interval", chunk_interval);
    
    /* chunk_size: 流式模型latency配置
     * [5,10,5] 表示:
     * - 当前音频600ms
     * - 回看300ms
     * - 前看300ms
     */
    cJSON *chunk_size = cJSON_CreateArray();
    /* 创建三个数字对象,分别表示:
     * num1 = 5: 当前音频帧长度为300ms
     * num2 = 10: 回看音频帧长度为600ms
     * num3 = 5: 前看音频帧长度为300ms
     */
    cJSON *num1 = cJSON_CreateNumber(5);
    cJSON *num2 = cJSON_CreateNumber(10); 
    cJSON *num3 = cJSON_CreateNumber(5);

    /* 将三个数字对象按顺序添加到chunk_size数组中
     * 最终形成[5,10,5]的数组结构
     * 这个配置用于控制流式识别的延迟
     */
    cJSON_AddItemToArray(chunk_size, num1);
    cJSON_AddItemToArray(chunk_size, num2);
    cJSON_AddItemToArray(chunk_size, num3);

    /* 将chunk_size数组添加到chunk_interval对象中
     * 键名为"chunk_size"
     */
    cJSON_AddItemToObject(data, "chunk_size", chunk_size);
    /* 添加热词及其权重 */
    char *hotwords_str = "{\"阿里巴巴\":20,\"hello world\":40}";
    cJSON_AddStringToObject(data, "hotwords", hotwords_str);

    /* 添加语音识别相关配置参数 */
    cJSON_AddBoolToObject(data, "is_speaking", true);
    cJSON_AddBoolToObject(data, "itn", true);
    cJSON_AddStringToObject(data, "mode", "2pass");
    cJSON_AddStringToObject(data, "wav_name", "wav-default-id");
    cJSON_AddStringToObject(data, "svs_lang", "auto");
    cJSON_AddStringToObject(data, "wav_format", "pcm");
    cJSON_AddNumberToObject(data, "audio_fs", 16000);

    
    char *json_str = cJSON_Print(data);
    ESP_LOGI(TAG, "Sending start frame: %s", json_str);
    
    int ret = esp_websocket_client_send_text(client, json_str, strlen(json_str), portMAX_DELAY);
    
    free(json_str);
    cJSON_Delete(data);
    
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

/* 发送结束帧 */
esp_err_t send_finish_frame() {
    if (client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(data, "is_speaking", false);
    cJSON_AddItemToObject(root, "data", data);
    
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "Sending finish frame: %s", json_str);
    
    int ret = esp_websocket_client_send_text(client, json_str, strlen(json_str), portMAX_DELAY);
    
    free(json_str);
    cJSON_Delete(root);
    
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}
/**
 * @brief 发送音频数据到WebSocket服务器
 * 
 * 该函数完成以下工作:
 * 1. 检查WebSocket客户端连接状态
 * 2. 以二进制格式发送音频数据
 * 
 * @param data 要发送的音频数据缓冲区
 * @param len 音频数据长度(字节)
 * @return esp_err_t ESP_OK:发送成功 ESP_FAIL:发送失败
 */
esp_err_t websocket_send_audio(const uint8_t *data, size_t len)
{
    /* 检查WebSocket客户端是否已连接 */
    if (!esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WebSocket client not connected");
        /* 延时1秒后再返回失败 */
        vTaskDelay(pdMS_TO_TICKS(3000));
        return ESP_FAIL;
    }

    // /* 打印发送的音频数据信息 */
    // ESP_LOGI(TAG, "正在发送音频数据, 长度: %d 字节", len);
    // ESP_LOGI(TAG, "音频数据前16字节: ");
    // for(int i = 0; i < 16 && i < len; i++) {
    //     printf("%02x ", data[i]);
    //     if((i + 1) % 8 == 0) printf("\n");
    // }
    // printf("\n");

    /* 以二进制格式发送音频数据
     * portMAX_DELAY表示无限等待直到发送完成
     */
    esp_err_t ret = esp_websocket_client_send_bin(client, (const char*)data, len, portMAX_DELAY);
    if (ret == -1) {
        ESP_LOGE(TAG, "Failed to send data, error code: %d", ret);
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief 清理并关闭WebSocket连接
 * 
 * 该函数完成以下工作:
 * 1. 停止WebSocket客户端
 * 2. 销毁WebSocket客户端实例
 * 3. 清空客户端指针
 */
void websocket_cleanup(void)
{
    /* 检查客户端是否存在 */
    if (client) {
        /* 停止WebSocket客户端 */
        esp_websocket_client_stop(client);
        /* 销毁WebSocket客户端实例,释放资源 */
        esp_websocket_client_destroy(client);
        /* 将客户端指针置空 */
        client = NULL;
    }
}



