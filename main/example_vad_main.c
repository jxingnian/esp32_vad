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
#include "esp_log.h"    // ESP日志系统
#include "board.h"      // 开发板配置
#include "audio_common.h" // 音频通用定义
#include "audio_pipeline.h" // 音频处理管道
#include "i2s_stream.h"  // I2S流处理
#include "raw_stream.h"  // 原始数据流处理
#include "filter_resample.h" // 重采样过滤器
#include "esp_vad.h"    // ESP VAD功能

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
static const char *TAG = "EXAMPLE-VAD";

/* VAD相关参数定义 */
#define VAD_SAMPLE_RATE_HZ 16000    // VAD采样率(Hz)
#define VAD_FRAME_LENGTH_MS 30      // VAD帧长度(毫秒)
#define VAD_BUFFER_LENGTH (VAD_FRAME_LENGTH_MS * VAD_SAMPLE_RATE_HZ / 1000) // VAD缓冲区长度

// 语音识别结果回调函数
static void speech_result_callback(const char *result, bool is_final) {
    if (is_final) {
        ESP_LOGI(TAG, "最终识别结果: %s", result);
    } else {
        ESP_LOGI(TAG, "中间识别结果: %s", result);
    }
}

void app_main()
{
    uint8_t voice_reading = 0;//语音识别状态
    /* 设置日志级别 - 移到最开始的位置 */
    esp_log_level_set("*", ESP_LOG_INFO);    // 所有组件设为信息级别
    esp_log_level_set(TAG, ESP_LOG_INFO);    // VAD示例设为信息级别

    // 初始化NVS Flash
    esp_err_t ret = nvs_flash_init();
    // 如果NVS分区没有足够空间或版本不匹配，则擦除重新初始化
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    // 检查NVS初始化结果
    ESP_ERROR_CHECK(ret);

    // 初始化网络接口
    ESP_ERROR_CHECK(esp_netif_init());

    // 初始化WiFi
    app_wifi_init();
    // 连接到指定WiFi网络
    app_wifi_connect(WIFI_SSID, WIFI_PASSWORD);

    /* 声明音频管道和音频元素句柄 */
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t i2s_stream_reader, filter, raw_read;

    /* 步骤1: 初始化编解码器芯片 */
    ESP_LOGI(TAG, "[ 1 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    /* 步骤2: 创建音频录制管道 */
    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline for recording");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    /* 步骤2.1: 创建I2S流读取器 */
    ESP_LOGI(TAG, "[2.1] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, 48000, 16, AUDIO_STREAM_READER);
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    /* 步骤2.2: 创建重采样过滤器 */
    ESP_LOGI(TAG, "[2.2] Create filter to resample audio data");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 48000;       // 源采样率
    rsp_cfg.src_ch = 2;             // 源通道数
    rsp_cfg.dest_rate = VAD_SAMPLE_RATE_HZ;  // 目标采样率
    rsp_cfg.dest_ch = 1;            // 目标通道数
    filter = rsp_filter_init(&rsp_cfg);

    /* 步骤2.3: 创建原始数据流接收器 */
    ESP_LOGI(TAG, "[2.3] Create raw to receive data");
    raw_stream_cfg_t raw_cfg = {
        .out_rb_size = 8 * 1024,    // 输出环形缓冲区大小
        .type = AUDIO_STREAM_READER, // 设置为读取器类型
    };
    raw_read = raw_stream_init(&raw_cfg);

    /* 步骤3: 注册所有元素到音频管道 */
    ESP_LOGI(TAG, "[ 3 ] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, filter, "filter");
    audio_pipeline_register(pipeline, raw_read, "raw");

    /* 步骤4: 链接各个元素 */
    ESP_LOGI(TAG, "[ 4 ] Link elements together [codec_chip]-->i2s_stream-->filter-->raw-->[VAD]");
    const char *link_tag[3] = {"i2s", "filter", "raw"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    /* 步骤5: 启动音频管道 */
    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    /* 步骤6: 初始化VAD句柄 */
    ESP_LOGI(TAG, "[ 6 ] Initialize VAD handle");
    vad_handle_t vad_inst = vad_create(VAD_MODE_3);  // 创建VAD实例，使用模式4(最严格)

    /* 分配VAD缓冲区内存 */
    int16_t *vad_buff = (int16_t *)malloc(VAD_BUFFER_LENGTH * sizeof(short));
    if (vad_buff == NULL) {
        ESP_LOGE(TAG, "Memory allocation failed!");
        goto abort_speech_detection;
    }
    // 等待WiFi连接
    while (!app_wifi_get_connect_status()) {
        ESP_LOGI(TAG, "等待WiFi连接...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // 获取IP地址
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &ip_info));
        ESP_LOGI(TAG, "WiFi已连接, IP地址: " IPSTR, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGE(TAG, "获取网络接口失败");
        goto abort_speech_detection;
    }
    websocket_init(WEBSOCKET_URI, false);
    vTaskDelay(pdMS_TO_TICKS(4000));
    send_start_frame();
    /* 主循环：持续进行语音检测 */
    while (1) {
        // 从原始数据流读取音频数据
        raw_stream_read(raw_read, (char *)vad_buff, VAD_BUFFER_LENGTH * sizeof(short));

        // 将音频样本送入VAD处理并获取结果
        vad_state_t vad_state = vad_process(vad_inst, vad_buff, VAD_SAMPLE_RATE_HZ, VAD_FRAME_LENGTH_MS);
        if (vad_state == VAD_SPEECH) {    // 开启语音识别
            // if(voice_reading == 0){
            //     voice_reading = 1;
            //     send_start_frame();
            // }
        }else{//没有检测到说话
        }
        websocket_send_audio( (const uint8_t *)vad_buff, VAD_BUFFER_LENGTH * sizeof(short));
    }
    websocket_cleanup();
    /* 释放VAD缓冲区 */
    free(vad_buff);
    vad_buff = NULL;

abort_speech_detection:

    /* 步骤7: 销毁VAD实例 */
    ESP_LOGI(TAG, "[ 7 ] Destroy VAD");
    vad_destroy(vad_inst);

    /* 步骤8: 停止音频管道并释放所有资源 */
    ESP_LOGI(TAG, "[ 8 ] Stop audio_pipeline and release all resources");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    /* 在移除监听器之前终止管道 */
    audio_pipeline_remove_listener(pipeline);

    /* 注销所有音频元素 */
    audio_pipeline_unregister(pipeline, i2s_stream_reader);
    audio_pipeline_unregister(pipeline, filter);
    audio_pipeline_unregister(pipeline, raw_read);

    /* 释放所有资源 */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(i2s_stream_reader);
    audio_element_deinit(filter);
    audio_element_deinit(raw_read);
}
