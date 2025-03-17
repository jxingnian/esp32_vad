idf_component_register(SRCS "ollama_main.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_client nvs_flash esp_wifi
                    PRIV_REQUIRES json) 