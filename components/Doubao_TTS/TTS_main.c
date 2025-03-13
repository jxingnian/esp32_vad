/*
 * 豆包TTS WebSocket客户端
 * 
 * 该模块实现了与豆包TTS服务器的WebSocket通信功能,包括:
 * - WebSocket连接的建立和管理
 * - 文本数据的发送
 * - 音频数据的接收和处理
 * 
 * 作者: 星年
 * 日期: 2024-03-13
 */

#include "doubao_tts.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"

/* 日志标签 */
static const char *TAG = "DOUBAO_TTS";

/* WebSocket客户端句柄 */
static esp_websocket_client_handle_t doubao_client;

/* 保存WebSocket连接参数的全局变量 */
static struct {
    char uri[128];
    bool is_ssl;
    char appid[64];
    char token[256];
} doubao_ws_config = {0};

/* 函数声明 */
static void doubao_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
esp_err_t doubao_websocket_init(const char *uri, bool is_ssl, const char *appid, const char *token);
esp_err_t doubao_send_tts_request(const char* text, const char* voice_name, float speed);
esp_err_t doubao_send_finish_frame(void);
void doubao_websocket_cleanup(void);

/* WebSocket事件处理函数
 * 该函数处理所有WebSocket相关事件,包括连接、断开连接、数据接收和错误
 * 
 * 参数:
 *   handler_args: 事件处理器的参数
 *   base: 事件基类型
 *   event_id: 事件ID,用于区分不同类型的事件
 *   event_data: 事件相关的数据
 */
static void doubao_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
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
            esp_err_t stop_err = esp_websocket_client_stop(doubao_client);
            if (stop_err != ESP_OK) {
                ESP_LOGE(TAG, "停止WebSocket客户端失败: %s", esp_err_to_name(stop_err));
            }
            
            /* 使用保存的配置重新初始化并启动客户端 */
            esp_websocket_client_config_t websocket_cfg = {0};  // 使用{0}初始化所有字段
            websocket_cfg.uri = doubao_ws_config.uri;
            websocket_cfg.disable_auto_reconnect = false;     /* 启用自动重连 */
            websocket_cfg.task_stack = 4096;                  /* WebSocket任务栈大小(字节) */
            websocket_cfg.task_prio = 5;                      /* WebSocket任务优先级(0-25,数字越大优先级越高) */
            websocket_cfg.buffer_size = 1024 * 4;            /* 收发数据缓冲区大小(字节)，增大以支持更多音频数据 */
            websocket_cfg.transport = doubao_ws_config.is_ssl ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP;
            websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;
            
            /* 构建认证头 */
            char auth_header[512];
            snprintf(auth_header, sizeof(auth_header), "Bearer; %s", doubao_ws_config.token);
            websocket_cfg.headers = auth_header;              /* 添加认证头 */
            
            /* 重新初始化客户端 */
            doubao_client = esp_websocket_client_init(&websocket_cfg);
            if (doubao_client == NULL) {
                ESP_LOGE(TAG, "重新初始化WebSocket客户端失败");
                break;
            }
            
            /* 重新注册事件处理函数 */
            esp_err_t reg_err = esp_websocket_register_events(doubao_client, WEBSOCKET_EVENT_ANY, doubao_websocket_event_handler, NULL);
            if (reg_err != ESP_OK) {
                ESP_LOGE(TAG, "注册事件处理函数失败: %s", esp_err_to_name(reg_err));
                break;
            }
            
            /* 启动客户端 */
            esp_err_t start_err = esp_websocket_client_start(doubao_client);
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
            } else if (data->op_code == 0x01) { // 文本帧
                /* 解析接收到的JSON数据 */
                cJSON *root = cJSON_Parse((char *)data->data_ptr);
                if (root == NULL) {
                    ESP_LOGE(TAG, "JSON解析失败");
                    break;
                }

                /* 获取响应状态 */
                cJSON *code = cJSON_GetObjectItem(root, "code");
                if (code && code->valueint != 0) {
                    ESP_LOGE(TAG, "TTS错误: code=%d, message=%s", 
                        code->valueint,
                        cJSON_GetObjectItem(root, "message")->valuestring);
                    cJSON_Delete(root);
                    break;
                }

                /* 获取响应类型 */
                cJSON *response_type = cJSON_GetObjectItem(root, "response_type");
                if (response_type) {
                    const char *type = response_type->valuestring;
                    if (strcmp(type, "MetaInfo") == 0) {
                        /* 元信息响应 */
                        ESP_LOGI(TAG, "收到元信息响应");
                        cJSON *meta_info = cJSON_GetObjectItem(root, "meta_info");
                        if (meta_info) {
                            ESP_LOGI(TAG, "音频格式: %s", 
                                cJSON_GetObjectItem(meta_info, "format")->valuestring);
                            ESP_LOGI(TAG, "采样率: %d", 
                                cJSON_GetObjectItem(meta_info, "sample_rate")->valueint);
                        }
                    } else if (strcmp(type, "TaskEnd") == 0) {
                        /* 任务结束响应 */
                        ESP_LOGI(TAG, "TTS合成任务完成");
                    }
                }

                cJSON_Delete(root);
            } else if (data->op_code == 0x02) { // 二进制帧
                /* 处理音频数据 */
                ESP_LOGI(TAG, "收到音频数据: %d字节", data->data_len);
                // TODO: 在这里处理音频数据,例如写入文件或播放
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

/* 发送开始帧 */
esp_err_t doubao_send_tts_request(const char* text, const char* voice_name, float speed) {
    if (doubao_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 生成唯一的reqid */
    char reqid[64];
    snprintf(reqid, sizeof(reqid), "esp32_tts_%llu", (unsigned long long)esp_timer_get_time());

    cJSON *root = cJSON_CreateObject();
    
    /* 添加app参数(必需) */
    cJSON *app = cJSON_CreateObject();
    cJSON_AddStringToObject(app, "appid", doubao_ws_config.appid);
    cJSON_AddStringToObject(app, "token", doubao_ws_config.token);
    cJSON_AddStringToObject(app, "cluster", "volcano_tts");
    cJSON_AddItemToObject(root, "app", app);

    /* 添加user参数(必需) */
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "uid", "esp32_user");
    cJSON_AddItemToObject(root, "user", user);

    /* 添加audio参数(必需) */
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "voice_type", voice_name);  // 音色代号
    cJSON_AddStringToObject(audio, "encoding", "pcm");         // 音频编码格式
    cJSON_AddNumberToObject(audio, "speed_ratio", speed);      // 语速
    cJSON_AddItemToObject(root, "audio", audio);

    /* 添加request参数(必需) */
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "reqid", reqid);         // 请求ID，需要保证唯一性
    cJSON_AddStringToObject(request, "text", text);           // 要合成的文本
    cJSON_AddStringToObject(request, "operation", "submit");   // 操作类型，必须为submit
    cJSON_AddItemToObject(root, "request", request);
    
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "发送TTS请求: %s", json_str);
    
    int ret = esp_websocket_client_send_text(doubao_client, json_str, strlen(json_str), portMAX_DELAY);
    
    free(json_str);
    cJSON_Delete(root);
    
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

/* 发送结束帧 */
esp_err_t doubao_send_finish_frame() {
    if (doubao_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "request_type", "TaskEnd");
    
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "发送结束帧: %s", json_str);
    
    int ret = esp_websocket_client_send_text(doubao_client, json_str, strlen(json_str), portMAX_DELAY);
    
    free(json_str);
    cJSON_Delete(root);
    
    return (ret > 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 清理并关闭WebSocket连接
 * 
 * 该函数完成以下工作:
 * 1. 停止WebSocket客户端
 * 2. 销毁WebSocket客户端实例
 * 3. 清空客户端指针
 */
void doubao_websocket_cleanup(void)
{
    /* 检查客户端是否存在 */
    if (doubao_client) {
        /* 停止WebSocket客户端 */
        esp_websocket_client_stop(doubao_client);
        /* 销毁WebSocket客户端实例,释放资源 */
        esp_websocket_client_destroy(doubao_client);
        /* 将客户端指针置空 */
        doubao_client = NULL;
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
 * @param uri WebSocket服务器地址
 * @param is_ssl 是否使用SSL连接
 * @param appid 应用ID
 * @param token 访问令牌
 * @return esp_err_t ESP_OK:成功 ESP_FAIL:失败
 */
esp_err_t doubao_websocket_init(const char *uri, bool is_ssl, const char *appid, const char *token)
{    
    /* 保存连接参数供后续使用 */
    strncpy(doubao_ws_config.uri, uri, sizeof(doubao_ws_config.uri) - 1);
    doubao_ws_config.uri[sizeof(doubao_ws_config.uri) - 1] = '\0';
    doubao_ws_config.is_ssl = is_ssl;
    
    strncpy(doubao_ws_config.appid, appid, sizeof(doubao_ws_config.appid) - 1);
    doubao_ws_config.appid[sizeof(doubao_ws_config.appid) - 1] = '\0';
    
    strncpy(doubao_ws_config.token, token, sizeof(doubao_ws_config.token) - 1);
    doubao_ws_config.token[sizeof(doubao_ws_config.token) - 1] = '\0';

    /* 构建认证头 */
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer; %s", token);

    /* 配置WebSocket客户端参数 */
    esp_websocket_client_config_t websocket_cfg = {0};  // 使用{0}初始化所有字段
    websocket_cfg.uri = doubao_ws_config.uri;
    websocket_cfg.disable_auto_reconnect = false;     /* 启用自动重连 */
    websocket_cfg.task_stack = 4096;                  /* WebSocket任务栈大小(字节) */
    websocket_cfg.task_prio = 5;                      /* WebSocket任务优先级(0-25,数字越大优先级越高) */
    websocket_cfg.buffer_size = 1024 * 4;            /* 收发数据缓冲区大小(字节)，增大以支持更多音频数据 */
    websocket_cfg.transport = doubao_ws_config.is_ssl ? WEBSOCKET_TRANSPORT_OVER_SSL : WEBSOCKET_TRANSPORT_OVER_TCP;
    websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    websocket_cfg.headers = auth_header;              /* 添加认证头 */

    /* 使用配置初始化WebSocket客户端 */
    doubao_client = esp_websocket_client_init(&websocket_cfg);
    if (doubao_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return ESP_FAIL;
    }

    /* 注册事件处理函数,处理所有WebSocket事件 */
    esp_websocket_register_events(doubao_client, WEBSOCKET_EVENT_ANY, doubao_websocket_event_handler, NULL);

    /* 启动WebSocket客户端,开始连接服务器 */
    esp_err_t ret = esp_websocket_client_start(doubao_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client");
        return ret;
    }

    return ESP_OK;
}



