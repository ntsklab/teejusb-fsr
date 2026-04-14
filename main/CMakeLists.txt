idf_component_register(
  SRCS "esp32c6_bridge_main.cpp"
  INCLUDE_DIRS "." "../esp32c6-bridge"
  REQUIRES esp_event esp_http_server esp_netif esp_wifi nvs_flash spiffs driver json
)

spiffs_create_partition_image(storage ../esp32c6-bridge/data FLASH_IN_PROJECT)