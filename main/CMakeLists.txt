idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  REQUIRES
    esp_wifi
    esp_netif
    esp_event
    nvs_flash
    lwip
    vfs
    led_strip
    esp_driver_gpio
    mdns
)
