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

#include "ollama_main.h"

#include "esp_tts.h"                  // 语音合成库头文件
#include "esp_tts_voice_xiaole.h"     // 小乐音色配置
#include "esp_tts_voice_template.h"   // 语音模板

#include "wav_encoder.h"              // WAV编码器
#include "esp_partition.h"            // 分区表操作
#include "esp_idf_version.h"          // ESP-IDF版本信息

/* 定义日志标签 */
static const char *TAG = "MIC-STREAM";

// I2S配置
#define I2S_SAMPLE_RATE     48000  // 输入采样率
#define TARGET_SAMPLE_RATE  16000  // 目标采样率
#define I2S_CHANNEL_NUM     1      // 单声道输入
#define I2S_BITS_PER_SAMPLE 16     // 每个采样16位

// I2S引脚定义
#define I2S_MIC_BCK_IO      5      // MIC BCK
#define I2S_MIC_WS_IO       4     // MIC WS
#define I2S_MIC_DATA_IO     6     // MIC DATA

#define I2S_SPK_BCK_IO      15     // 喇叭 BCK
#define I2S_SPK_WS_IO       16     // 喇叭 WS/LRC
#define I2S_SPK_DATA_IO     7      // 喇叭 DATA/DIN

// 缓冲区大小优化
#define CHUNK_SIZE          960    // 每个数据块的大小
#define BUFFER_SIZE         (CHUNK_SIZE * 3)  // 缓冲区大小为数据块的3倍
#define RESAMPLED_POINTS    (CHUNK_SIZE * TARGET_SAMPLE_RATE / I2S_SAMPLE_RATE)  // 重采样后的点数
#define RESAMPLED_BUFFER_SIZE (BUFFER_SIZE * TARGET_SAMPLE_RATE / I2S_SAMPLE_RATE + CHUNK_SIZE)  // 增加额外的CHUNK_SIZE作为安全边界

// 定义I2S端口
#define I2S_MIC_PORT       I2S_NUM_0
#define I2S_SPK_PORT       I2S_NUM_1

// 全局TTS句柄
static esp_tts_handle_t *g_tts_handle = NULL;

// Ollama响应回调函数
static void ollama_response_handler(const char *response)
{
    if (!response) {
        return;
    }
    
    ESP_LOGI(TAG, "收到Ollama响应: %s", response);
    
    // 如果TTS句柄有效，使用TTS播放响应
    if (g_tts_handle) {
        // 分配缓冲区用于播放
        int16_t *audio_buffer = malloc(8192 * sizeof(int16_t));
        if (!audio_buffer) {
            ESP_LOGE(TAG, "无法分配音频缓冲区");
            return;
        }
        
        // 解析中文文本并播放
        if (esp_tts_parse_chinese(g_tts_handle, response)) {
            int len[1] = {0};
            size_t bytes_written = 0;
            
            do {
                short *pcm_data = esp_tts_stream_play(g_tts_handle, len, 1);
                if (pcm_data && len[0] > 0) {
                    // 复制数据到缓冲区并通过I2S播放
                    memcpy(audio_buffer, pcm_data, len[0] * 2);
                    i2s_write(I2S_SPK_PORT, audio_buffer, len[0] * 2, &bytes_written, 100 / portTICK_PERIOD_MS);
                }
            } while (len[0] > 0);
        }
        
        // 重置TTS流
        esp_tts_stream_reset(g_tts_handle);
        free(audio_buffer);
    }
}

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

    // 分配缓冲区
    // 分配原始音频数据缓冲区,大小为 BUFFER_SIZE * 2 字节(每个采样点16位)
    int16_t *raw_buffer = (int16_t *)malloc(BUFFER_SIZE * sizeof(int16_t));
    
    // 分配重采样后的音频数据缓冲区,大小为 RESAMPLED_BUFFER_SIZE * 2 字节
    // 缓冲区大小根据重采样比例调整,并额外增加一个CHUNK_SIZE作为安全边界
    int16_t *resampled_buffer = (int16_t *)malloc(RESAMPLED_BUFFER_SIZE * sizeof(int16_t));
    
    // 用于跟踪实际读取的字节数
    size_t bytes_read = 0;
    
    // 用于跟踪实际写入的字节数 
    size_t bytes_written = 0;

    if (!raw_buffer || !resampled_buffer) {
        ESP_LOGE(TAG, "内存分配失败!");
        goto cleanup;
    }

    // MIC I2S配置
    i2s_config_t i2s_mic_config = {
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

    // 喇叭I2S配置
    i2s_config_t i2s_spk_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = 24000,  // 设置为24kHz，与TTS服务返回的音频匹配
        .bits_per_sample = I2S_BITS_PER_SAMPLE,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = true,  // 使用APLL获得更准确的时钟
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    // MIC I2S引脚配置
    i2s_pin_config_t mic_pin_config = {
        .bck_io_num = I2S_MIC_BCK_IO,
        .ws_io_num = I2S_MIC_WS_IO,
        .data_out_num = -1,
        .data_in_num = I2S_MIC_DATA_IO
    };

    // 喇叭I2S引脚配置
    i2s_pin_config_t spk_pin_config = {
        .bck_io_num = I2S_SPK_BCK_IO,
        .ws_io_num = I2S_SPK_WS_IO,
        .data_out_num = I2S_SPK_DATA_IO,
        .data_in_num = -1
    };

    // 初始化I2S
    ESP_ERROR_CHECK(i2s_driver_install(I2S_MIC_PORT, &i2s_mic_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_MIC_PORT, &mic_pin_config));
    ESP_ERROR_CHECK(i2s_driver_install(I2S_SPK_PORT, &i2s_spk_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_SPK_PORT, &spk_pin_config));

    // 等待WiFi连接
    while (!app_wifi_get_connect_status()) {
        ESP_LOGI(TAG, "等待WiFi连接...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /*** 1. 创建语音合成句柄 ***/
    // 从voice_data分区加载语音数据
    // 查找名为"voice_data"的数据分区
    const esp_partition_t* part=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (part==NULL) { 
        printf("Couldn't find voice data partition!\n"); // 找不到voice_data分区时报错
    } else {
        printf("voice_data paration size:%ld\n", part->size); // 打印分区大小
    }
    void* voicedata; // 用于存储语音数据的指针
    
    // 用于存储内存映射句柄
    esp_partition_mmap_handle_t mmap;
    // 将分区数据映射到内存中,便于访问
    esp_err_t err=esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &voicedata, &mmap);

    if (err != ESP_OK) {
        printf("Couldn't map voice data partition!\n"); // 内存映射失败时报错
    }
    // 使用语音模板和映射的语音数据初始化语音配置
    esp_tts_voice_t *voice=esp_tts_voice_set_init(&esp_tts_voice_xiaole, (int16_t*)voicedata); 
    
    // 使用初始化好的语音配置创建TTS句柄,用于后续语音合成
    g_tts_handle = esp_tts_create(voice);

    /*** 2. 播放欢迎提示语 ***/
    char *prompt1="你好,我是小豆包";  // 定义欢迎语
    printf("%s\n", prompt1); // 在控制台打印欢迎语
    // 解析中文文本
    if (esp_tts_parse_chinese(g_tts_handle, prompt1)) {
            int len[1]={0}; // 用于存储生成的音频数据长度
            do {
                // 获取合成的PCM音频数据流
                short *pcm_data = esp_tts_stream_play(g_tts_handle, len, 2);
                // esp_audio_play(pcm_data, len[0]*2, portMAX_DELAY); // 注释掉的音频播放接口
                
                // 将PCM数据复制到原始缓冲区
                memcpy(raw_buffer, pcm_data, len[0]*2);
                // 通过I2S接口输出音频数据到扬声器
                i2s_write(I2S_SPK_PORT, raw_buffer, len[0]*2, &bytes_written, 100 / portTICK_PERIOD_MS);
            } while(len[0]>0); // 持续处理直到所有音频数据都播放完
    }
    // 重置TTS流,为下一次合成做准备
    esp_tts_stream_reset(g_tts_handle);
    
    // 初始化Ollama客户端
    ESP_ERROR_CHECK(ollama_init(OLLAMA_URI));
    
    // 设置Ollama响应回调函数
    ollama_set_response_callback(ollama_response_handler);

    // 初始化WebSocket连接
    funasr_websocket_init(FUNASR_WEBSOCKET_URI, false);
    vTaskDelay(pdMS_TO_TICKS(3000));
    funasr_send_start_frame();
    // ollama_chat("你好");
    // 主循环
    while (1) {
        // 读取I2S数据
        esp_err_t ret = i2s_read(I2S_MIC_PORT, raw_buffer, BUFFER_SIZE * sizeof(int16_t), &bytes_read, 100 / portTICK_PERIOD_MS);
        
        if (ret == ESP_OK && bytes_read > 0) {
            // 直接播放原始音频
            // i2s_write(I2S_SPK_PORT, raw_buffer, bytes_read, &bytes_written, 100 / portTICK_PERIOD_MS);
            
            // 重采样用于发送
            resample_data(raw_buffer, bytes_read / sizeof(int16_t), resampled_buffer, I2S_CHANNEL_NUM);
            
            // 计算实际重采样后的样本数
            size_t samples = (bytes_read / sizeof(int16_t)) * TARGET_SAMPLE_RATE / I2S_SAMPLE_RATE;
            size_t remaining_samples = samples;
            size_t offset = 0;
            
            // 当累积了足够的数据时发送
            while (remaining_samples >= CHUNK_SIZE) {
                // 发送到服务器
                funasr_websocket_send_audio((const uint8_t *)(resampled_buffer + offset), CHUNK_SIZE * sizeof(int16_t));
                
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
    i2s_driver_uninstall(I2S_MIC_PORT);
    i2s_driver_uninstall(I2S_SPK_PORT);
    funasr_websocket_cleanup();
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
