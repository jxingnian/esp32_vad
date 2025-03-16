
/*
 * @Author: xingnian j_xingnian@163.com
 * @Date: 2025-03-10 13:24:23
 * @LastEditors: xingnian j_xingnian@163.com
 * @LastEditTime: 2025-03-10 16:03:23
 * @FilePath: \esp32_vad\main\const.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
 
main目录下添加 const.h 文件

#ifndef CONST_H
#define CONST_H

#define WIFI_SSID "ssid"
#define WIFI_PASSWORD "password"

#define WEBSOCKET_URI "your_url"

#endif


部署语音转文字-参考文档
https://github.com/modelscope/FunASR/blob/main/runtime/docs/websocket_protocol_zh.md

豆包-参考文档
https://www.volcengine.com/docs/6561/1257584

websocket-参考文档
https://docs.espressif.com/projects/esp-protocols/esp_websocket_client/docs/latest/index.html

