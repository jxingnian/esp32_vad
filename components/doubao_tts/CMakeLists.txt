# 注册组件
idf_component_register(
    # 源文件列表
    SRCS 
        "doubao_main.c"
    # 包含目录
    INCLUDE_DIRS 
        "."              # 头文件目录
    # 公共依赖组件
    REQUIRES         
        esp_websocket_client
        esp_http_client
        json
        mbedtls
        esp-tls
)
