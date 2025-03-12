/* 语音活动检测(VAD)示例

   本示例代码属于公共领域(或根据您的选择采用CC0许可)。

   除非适用法律要求或书面同意，本软件按"原样"分发，
   不附带任何明示或暗示的担保或条件。
*/

/* 包含必要的头文件 */
#include <stdio.h>      // 标准输入输出
#include <string.h>     // 字符串操作
#include <stdlib.h>     // 标准库函数
#include "freertos/FreeRTOS.h"  // FreeRTOS操作系统
#include "freertos/task.h"
#include "esp_log.h"    // ESP日志系统
#include "driver/i2s.h"

#include "audio_idf_version.h" // IDF版本信息
#include "const.h"
// ESP32网络接口头文件
#include "esp_netif.h"
// ESP32非易失性存储头文件
#include "nvs_flash.h"
// WiFi应用头文件
#include "app_wifi.h"
#include "esp_timer.h"  // 添加ESP定时器头文件
#include "funasr_main.h"

/* 定义日志标签 */
static const char *TAG = "MIC-STREAM";

// I2S配置
#define I2S_SAMPLE_RATE     48000  // 输入采样率
#define TARGET_SAMPLE_RATE  16000  // 目标采样率
#define I2S_CHANNEL_NUM     1      // 单声道输入
#define I2S_BITS_PER_SAMPLE 16     // 每个采样16位

// 缓冲区大小优化
#define CHUNK_SIZE          960    // 每个数据块的大小
#define BUFFER_SIZE         (CHUNK_SIZE * 3)  // 缓冲区大小为数据块的3倍
#define RESAMPLED_POINTS    (CHUNK_SIZE * TARGET_SAMPLE_RATE / I2S_SAMPLE_RATE)  // 重采样后的点数
#define RESAMPLED_BUFFER_SIZE (BUFFER_SIZE * TARGET_SAMPLE_RATE / I2S_SAMPLE_RATE + CHUNK_SIZE)  // 增加额外的CHUNK_SIZE作为安全边界

// 重采样函数优化
static void resample_data(int16_t *input, int input_len, int16_t *output, int channels) {
    int step = (I2S_SAMPLE_RATE / TARGET_SAMPLE_RATE);
    int j = 0;
    
    // 简单抽取重采样
    for (int i = 0; i < input_len; i += step) {
        output[j++] = input[i];
    }
}

// 音频采集任务
static void mic_task(void *arg) {
    // I2S配置
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = I2S_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // 单声道
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,        // 增加DMA缓冲区数量
        .dma_buf_len = 1024,       // 增加DMA缓冲区长度
        .use_apll = true,          // 使用APLL提供更精确的采样率
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    // I2S引脚配置
    i2s_pin_config_t pin_config = {
        .bck_io_num = 9,     // BCK引脚
        .ws_io_num = 45,     // WS引脚
        .data_out_num = -1,  // 不使用输出
        .data_in_num = 10    // DATA引脚
    };

    // 初始化I2S
    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));

    // 分配缓冲区
    int16_t *raw_buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    int16_t *resampled_buffer = (int16_t *)malloc(RESAMPLED_BUFFER_SIZE * sizeof(int16_t));
    size_t bytes_read = 0;  // 添加 bytes_read 变量声明

    if (!raw_buffer || !resampled_buffer) {
        ESP_LOGE(TAG, "内存分配失败!");
        goto cleanup;
    }

    // 等待WiFi连接
    while (!app_wifi_get_connect_status()) {
        ESP_LOGI(TAG, "等待WiFi连接...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 初始化WebSocket连接
    websocket_init(WEBSOCKET_URI, false);
    vTaskDelay(pdMS_TO_TICKS(2000));
    send_start_frame();

    // 主循环
    while (1) {
        // 读取I2S数据
        esp_err_t ret = i2s_read(I2S_NUM_0, raw_buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_read, 100 / portTICK_PERIOD_MS);
        
        if (ret == ESP_OK && bytes_read > 0) {
            // 重采样
            resample_data(raw_buffer, bytes_read / sizeof(int16_t), resampled_buffer, I2S_CHANNEL_NUM);
            
            // 计算实际重采样后的样本数
            size_t samples = (bytes_read / sizeof(int16_t)) * TARGET_SAMPLE_RATE / I2S_SAMPLE_RATE;
            size_t remaining_samples = samples;
            size_t offset = 0;
            
            // 当累积了足够的数据时发送
            while (remaining_samples >= CHUNK_SIZE) {
                // 发送到服务器
                websocket_send_audio((const uint8_t *)(resampled_buffer + offset), CHUNK_SIZE * sizeof(int16_t));
                
                offset += CHUNK_SIZE;
                remaining_samples -= CHUNK_SIZE;
            }
            
            // 如果还有剩余数据，移动到缓冲区开始位置
            if (remaining_samples > 0 && offset > 0) {
                memmove(resampled_buffer, resampled_buffer + offset, remaining_samples * sizeof(int16_t));
            }
        }

        // 让出一些CPU时间
        vTaskDelay(1);
    }

cleanup:
    // 清理资源
    if (raw_buffer) free(raw_buffer);
    if (resampled_buffer) free(resampled_buffer);
    i2s_driver_uninstall(I2S_NUM_0);
    websocket_cleanup();
    vTaskDelete(NULL);
}

void app_main()
{
    // 设置日志级别
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_INFO);

    // 初始化NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化网络
    ESP_ERROR_CHECK(esp_netif_init());

    // 初始化WiFi
    app_wifi_init();
    app_wifi_connect(WIFI_SSID, WIFI_PASSWORD);

    // 创建音频采集任务，增加堆栈大小
    xTaskCreate(mic_task, "mic_task", 8192 * 2, NULL, 5, NULL);

    // 主循环
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000)); // 让主任务休眠，避免看门狗超时
    }
}
