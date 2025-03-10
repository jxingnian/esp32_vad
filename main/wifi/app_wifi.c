/*
 * @Author: HoGC
 * @Date: 2022-04-16 13:31:43
 * @Last Modified time: 2022-04-16 13:31:43
 */
#include "app_wifi.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;

static bool g_wifi_connect_status = false;
static bool g_has_smartconfig_mode = false;

static const char *TAG = "app_wifi";

static void _event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(!g_has_smartconfig_mode){
            esp_wifi_connect();
        }
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
        g_wifi_connect_status = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        g_wifi_connect_status = true;
        
        // WiFi 连接成功后同步时间
        ESP_LOGI(TAG, "初始化 SNTP");
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "pool.ntp.org");
        sntp_setservername(1, "time.apple.com");
        sntp_init();
        
        // 等待时间同步
        time_t now = 0;
        struct tm timeinfo = { 0 };
        int retry = 0;
        const int retry_count = 10;
        
        while (timeinfo.tm_year < (2020 - 1900) && ++retry < retry_count) {
            ESP_LOGI(TAG, "等待系统时间设置... (%d/%d)", retry, retry_count);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            time(&now);
            localtime_r(&now, &timeinfo);
        }
        
        if (timeinfo.tm_year >= (2020 - 1900)) {
            ESP_LOGI(TAG, "时间已同步");
            char strftime_buf[64];
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "当前时间: %s", strftime_buf);
        } else {
            ESP_LOGW(TAG, "时间同步失败");
        }
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "Scan done");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "Found channel");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "Got SSID and password");

        smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        uint8_t ssid[33] = { 0 };
        uint8_t password[65] = { 0 };

        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        wifi_config.sta.bssid_set = evt->bssid_set;
        if (wifi_config.sta.bssid_set == true) {
            memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
        }

        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SSID:%s", ssid);
        ESP_LOGI(TAG, "PASSWORD:%s", password);

        esp_wifi_set_mode(WIFI_MODE_STA);
        ESP_ERROR_CHECK( esp_wifi_disconnect() );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}
 
/**
 * @brief SmartConfig配置任务函数
 * 
 * 该任务负责执行ESP32的SmartConfig配置流程,包括:
 * 1. 启动SmartConfig配置
 * 2. 等待WiFi连接和配置完成
 * 3. 完成后清理并退出
 *
 * @param arg 任务参数(未使用)
 */
static void _sc_task(void* arg)
{
    // 用于存储事件组标志位
    EventBits_t uxBits;
    
    // 设置SmartConfig模式标志
    g_has_smartconfig_mode = true;
 
    // 设置SmartConfig类型为ESPTOUCH_AIRKISS
    ESP_ERROR_CHECK( esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS) );
    
    // 使用默认配置初始化SmartConfig
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    
    // 启动SmartConfig
    ESP_ERROR_CHECK( esp_smartconfig_start(&cfg) );
    
    // 等待配置完成
    while (1) {
        // 等待WiFi连接或SmartConfig完成事件
        // CONNECTED_BIT: WiFi已连接
        // ESPTOUCH_DONE_BIT: SmartConfig配置完成
        uxBits = xEventGroupWaitBits(s_wifi_event_group, 
                                   CONNECTED_BIT | ESPTOUCH_DONE_BIT, 
                                   true,    // 清除触发的标志位
                                   false,   // 任一标志位触发即返回
                                   portMAX_DELAY);  // 永久等待
                                   
        // 检查WiFi连接状态
        if(uxBits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi Connected to ap");
        }
        
        // 检查SmartConfig是否完成
        if(uxBits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(TAG, "smartconfig over");
            // 停止SmartConfig
            esp_smartconfig_stop();
            // 清除SmartConfig模式标志
            g_has_smartconfig_mode = false;
            // 删除当前任务
            vTaskDelete(NULL);
        }
    }
}


void app_wifi_smartconfig_start(void)
{
    xTaskCreate(_sc_task, "_sc_task", 2048, NULL, 3, NULL);
}

bool app_wifi_get_connect_status(void){
    return g_wifi_connect_status;
}

void app_wifi_connect(const char *ssid, const char *password){
    wifi_config_t wifi_config;

    if(!ssid){
        return;
    }

    bzero(&wifi_config, sizeof(wifi_config_t));
    strcpy((char *)wifi_config.sta.ssid, ssid);
    if(password != NULL && password[0] != '\0'){
        strcpy((char *)wifi_config.sta.password, password);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }else{
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "SSID:%s", ssid);
    ESP_LOGI(TAG, "PASSWORD:%s", password);

    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK( esp_wifi_disconnect() );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    esp_wifi_connect();
}
/**
 * @brief 初始化WiFi功能
 * 
 * 该函数完成WiFi功能的初始化,包括:
 * 1. 创建WiFi事件组
 * 2. 创建默认事件循环
 * 3. 创建并初始化WiFi Station和AP接口
 * 4. 初始化WiFi驱动
 * 5. 注册WiFi相关事件处理程序
 * 6. 启动WiFi
 */
void app_wifi_init(void)
{
    // 打印初始化日志
    ESP_LOGI(TAG, "app_init_wifi");
    
    // 创建WiFi事件组,用于事件同步
    s_wifi_event_group = xEventGroupCreate();
    
    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 创建并初始化WiFi Station接口
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    
    // 创建并初始化WiFi AP接口
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    // 使用默认配置初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );

    // 注册WiFi事件处理程序
    ESP_ERROR_CHECK( esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &_event_handler, NULL) );
    
    // 注册IP事件处理程序
    ESP_ERROR_CHECK( esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &_event_handler, NULL) );
    
    // 注册SmartConfig事件处理程序
    ESP_ERROR_CHECK( esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &_event_handler, NULL) );

    // 启动WiFi
    ESP_ERROR_CHECK( esp_wifi_start() );
}
