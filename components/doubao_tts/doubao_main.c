/*
 * 豆包TTS WebSocket客户端
 * 
 * 该模块实现了与豆包TTS服务的WebSocket通信功能,包括:
 * - WebSocket连接的建立和管理
 * - TTS请求的发送
 * - 音频数据的接收和处理
 * 
 * 作者: 星年
 * 日期: 2025-03-20
 */

#include "doubao_main.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"

/* 日志标签 */
static const char *TAG = "DOUBAO_TTS";

/* WebSocket客户端句柄 */
static esp_websocket_client_handle_t client = NULL;

/* 音频回调函数 */
static doubao_audio_callback_t audio_callback = NULL;

/* TTS请求参数 */
static struct {
    char appid[32];
    char token[128];
    char cluster[32];
    char voice_type[32];
} tts_config = {
    .appid = "2013524180",
    .token = "32jGMq9t0hZz9nZpzKt1ZwLro-aTvY8W",
    .cluster = "volcano_tts",
    .voice_type = "zh_female_sajiaonvyou_moon_bigtts"
};

/* 默认二进制协议头 - 显式设置为无压缩 */
static const uint8_t default_header[] = {0x11, 0x10, 0x10, 0x00};
/* 协议解析:
 * 0x11: 协议版本(0001) + 报头大小(0001) = 4字节
 * 0x10: 消息类型(0001) + 特定标志(0000)
 * 0x10: 序列化方法(0001=JSON) + 压缩方法(0000=无压缩)
 * 0x00: 保留字段
 */

/* 音频数据缓冲区结构 */
typedef struct audio_buffer {
    uint8_t *data;
    size_t size;
    int32_t sequence;
    struct audio_buffer *next;
} audio_buffer_t;

#define MIN_BUFFER_MS 500  // 最小缓冲时长(毫秒)
#define AUDIO_SAMPLE_RATE 24000  // 音频采样率
#define AUDIO_BYTES_PER_MS ((AUDIO_SAMPLE_RATE * 2) / 1000)  // 每毫秒的字节数 (16位采样)

/* 音频队列 */
static struct {
    audio_buffer_t *head;
    audio_buffer_t *tail;
    int32_t next_sequence;
    SemaphoreHandle_t mutex;
    QueueHandle_t play_queue;
    TaskHandle_t player_task;
    bool is_playing;
    size_t buffered_size;  // 已缓冲的数据大小
} audio_queue = {NULL, NULL, 0, NULL, NULL, NULL, false, 0};

/* 音频播放任务 */
static void audio_player_task(void *arg) {
    bool waiting_for_buffer = true;
    
    while (1) {
        size_t queued_size = 0;
        
        // 计算当前缓冲的数据量
        if (xSemaphoreTake(audio_queue.mutex, portMAX_DELAY)) {
            queued_size = audio_queue.buffered_size;
            xSemaphoreGive(audio_queue.mutex);
        }
        
        // 如果正在等待缓冲且数据不足，继续等待
        if (waiting_for_buffer) {
            if (queued_size < (MIN_BUFFER_MS * AUDIO_BYTES_PER_MS)) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            waiting_for_buffer = false;
            ESP_LOGI(TAG, "缓冲区已满，开始播放");
        }
        
        // 获取并播放音频数据
        audio_buffer_t *buffer = NULL;
        if (xQueueReceive(audio_queue.play_queue, &buffer, portMAX_DELAY)) {
            if (buffer && buffer->data && buffer->size > 0) {
                // 播放音频数据
                if (audio_callback) {
                    audio_callback(buffer->data, buffer->size);
                }
                
                // 更新缓冲区大小
                if (xSemaphoreTake(audio_queue.mutex, portMAX_DELAY)) {
                    if (audio_queue.buffered_size >= buffer->size) {
                        audio_queue.buffered_size -= buffer->size;
                    }
                    xSemaphoreGive(audio_queue.mutex);
                }
                
                // 释放缓冲区
                free(buffer->data);
                free(buffer);
            }
        }
        
        // 如果缓冲区为空，重置等待状态
        if (queued_size == 0) {
            waiting_for_buffer = true;
            ESP_LOGI(TAG, "缓冲区已空，等待新数据");
        }
        
        // 让出CPU时间
        vTaskDelay(1);
    }
}

/* 初始化音频队列 */
static void init_audio_queue(void) {
    audio_queue.head = NULL;
    audio_queue.tail = NULL;
    audio_queue.next_sequence = 0;
    
    if (audio_queue.mutex == NULL) {
        audio_queue.mutex = xSemaphoreCreateMutex();
    }
    
    if (audio_queue.play_queue == NULL) {
        audio_queue.play_queue = xQueueCreate(32, sizeof(audio_buffer_t *));
        xTaskCreate(audio_player_task, "audio_player", 4096, NULL, 5, NULL);
    }
}

/* 处理队列中的音频数据 */
static void process_audio_queue(void) {
    if (xSemaphoreTake(audio_queue.mutex, portMAX_DELAY)) {
        while (audio_queue.head && audio_queue.head->sequence == audio_queue.next_sequence) {
            audio_buffer_t *buffer = audio_queue.head;
            audio_queue.head = buffer->next;
            if (!audio_queue.head) {
                audio_queue.tail = NULL;
            }
            
            // 发送到播放队列
            if (buffer && buffer->size > 0) {
                if (xQueueSend(audio_queue.play_queue, &buffer, pdMS_TO_TICKS(100)) != pdPASS) {
                    ESP_LOGW(TAG, "播放队列已满，丢弃数据: 序列号=%ld", (long)buffer->sequence);
                    free(buffer->data);
                    free(buffer);
                } else {
                    // 更新缓冲区大小
                    audio_queue.buffered_size += buffer->size;
                    ESP_LOGD(TAG, "音频数据已加入播放队列: 序列号=%ld, 大小=%u, 总缓冲=%u", 
                            (long)buffer->sequence, buffer->size, audio_queue.buffered_size);
                }
            }
            
            audio_queue.next_sequence++;
        }
        xSemaphoreGive(audio_queue.mutex);
    }
}

/* 重置音频队列 */
static void reset_audio_queue(void) {
    if (xSemaphoreTake(audio_queue.mutex, portMAX_DELAY)) {
        audio_queue.buffered_size = 0;
        // ... 其他重置代码 ...
        xSemaphoreGive(audio_queue.mutex);
    }
}

/* WebSocket事件处理函数 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0) {
                /* 解析协议头 */
                uint8_t version = (data->data_ptr[0] >> 4) & 0x0F;    // 版本号
                uint8_t header_size = data->data_ptr[0] & 0x0F;       // 头部大小
                uint8_t msg_type = (data->data_ptr[1] >> 4) & 0x0F;   // 消息类型
                uint8_t flags = data->data_ptr[1] & 0x0F;             // 标志位
                
                /* 处理音频响应 */
                if (msg_type == 0xB && header_size == 0x1) {
                    if (flags == 0x0) {
                        ESP_LOGI(TAG, "收到服务器ACK");
                    } 
                    else if (flags == 0x1 || flags == 0x2) {
                        const unsigned char *payload = (const unsigned char *)data->data_ptr + (header_size * 4);
                        int32_t sequence = (payload[0] << 24) | (payload[1] << 16) | 
                                         (payload[2] << 8) | payload[3];
                        uint32_t payload_size = (payload[4] << 24) | (payload[5] << 16) | 
                                              (payload[6] << 8) | payload[7];
                        const unsigned char *audio_data = payload + 8;
                        
                        ESP_LOGI(TAG, "音频数据: 序列号=%ld, 大小=%lu字节", (long)sequence, (unsigned long)payload_size);
                        
                        /* 检查实际数据大小 */
                        size_t actual_size = data->data_len - (header_size * 4) - 8;
                        if (actual_size > 0) {
                            /* 调用音频回调处理数据 */
                            if (audio_callback) {
                                audio_callback(audio_data, actual_size);
                            }
                        }
                    }
                    else if (flags == 0x3) {
                        ESP_LOGI(TAG, "收到最后一个音频包");
                        if (data->data_len > (header_size * 4) + 8) {
                            const unsigned char *payload = (const unsigned char *)data->data_ptr + (header_size * 4);
                            const unsigned char *audio_data = payload + 8;
                            size_t actual_size = data->data_len - (header_size * 4) - 8;
                            
                            if (actual_size > 0 && audio_callback) {
                                audio_callback(audio_data, actual_size);
                            }
                        }
                    }
                }
            }
            break;
            
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket连接成功");
            break;
            
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket断开连接");
            break;
            
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket错误");
            break;
            
        default:
            break;
    }
}

/* 设置音频回调函数 */
void doubao_tts_set_audio_callback(doubao_audio_callback_t callback)
{
    audio_callback = callback;
}

/* 初始化TTS服务 */
esp_err_t doubao_tts_init(const char *uri)
{
    esp_websocket_client_config_t websocket_cfg = {0};
    websocket_cfg.uri = uri;
    websocket_cfg.disable_auto_reconnect = false;
    websocket_cfg.task_stack = 8192;
    websocket_cfg.buffer_size = 16384;  // 增加缓冲区大小到16KB
    websocket_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
    websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    
    static const char *auth_header = "Authorization: Bearer; 32jGMq9t0hZz9nZpzKt1ZwLro-aTvY8W\r\n";
    websocket_cfg.headers = auth_header;

    client = esp_websocket_client_init(&websocket_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "初始化WebSocket客户端失败");
        return ESP_FAIL;
    }
    
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    
    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动WebSocket客户端失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "豆包TTS初始化完成");
    return ESP_OK;
}

/* 发送TTS请求 */
esp_err_t doubao_tts_request(const char *text, const char *voice_type)
{
    if (client == NULL || !esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "WebSocket未连接");
        return ESP_FAIL;
    }
    
    // 生成随机请求ID
    char reqid[32];
    snprintf(reqid, sizeof(reqid), "esp32-%08lx-%08lx", esp_random(), esp_random());
    
    // 构建JSON请求
    cJSON *root = cJSON_CreateObject();
    
    cJSON *app = cJSON_CreateObject();
    cJSON_AddStringToObject(app, "appid", tts_config.appid);
    cJSON_AddStringToObject(app, "token", tts_config.token);
    cJSON_AddStringToObject(app, "cluster", tts_config.cluster);
    cJSON_AddItemToObject(root, "app", app);
    
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "uid", "esp32_user");
    cJSON_AddItemToObject(root, "user", user);
    
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "voice_type", voice_type ? voice_type : tts_config.voice_type);  // 必选
    cJSON_AddItemToObject(root, "audio", audio);
    
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "reqid", reqid);     // 必选
    cJSON_AddStringToObject(request, "text", text);       // 必选
    cJSON_AddStringToObject(request, "operation", "submit");  // 必选
    cJSON_AddItemToObject(root, "request", request);
    
    // 序列化JSON
    char *json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, "发送请求: %s", json_str);

    // 直接使用JSON字符串，无需压缩
    uint8_t *payload = (uint8_t *)json_str;
    size_t payload_len = strlen(json_str);
    
    // 构建二进制请求头
    uint8_t *full_request = malloc(4 + 4 + payload_len);
    if (!full_request) {
        ESP_LOGE(TAG, "内存分配失败");
        cJSON_Delete(root);
        free(json_str);
        return ESP_ERR_NO_MEM;
    }
    
    // 复制头部
    memcpy(full_request, default_header, 4);
    
    // 添加payload大小 (4字节，大端序)
    full_request[4] = (payload_len >> 24) & 0xFF;
    full_request[5] = (payload_len >> 16) & 0xFF;
    full_request[6] = (payload_len >> 8) & 0xFF;
    full_request[7] = payload_len & 0xFF;
    
    // 添加payload
    memcpy(full_request + 8, payload, payload_len);
    
    // 发送完整请求
    int ret = esp_websocket_client_send_bin(client, (const char *)full_request, 8 + payload_len, portMAX_DELAY);
    
    // 释放资源
    free(full_request);
    free(json_str);
    cJSON_Delete(root);
    
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

/* 清理资源 */
void doubao_tts_cleanup(void)
{
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
        client = NULL;
    }
}

