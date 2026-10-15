set(vendor "${CMAKE_CURRENT_LIST_DIR}/../../vendor")
idf_component_register(SRCS
    "${vendor}/gfx/Adafruit_GFX.cpp" "${vendor}/gfx/Adafruit_GrayOLED.cpp"
    "${vendor}/gfx/Adafruit_SPITFT.cpp" "${vendor}/gfx/glcdfont.c"
    "${vendor}/busio/Adafruit_BusIO_Register.cpp" "${vendor}/busio/Adafruit_GenericDevice.cpp"
    "${vendor}/busio/Adafruit_I2CDevice.cpp" "${vendor}/busio/Adafruit_SPIDevice.cpp"
    "${vendor}/hub75/src/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp"
    "${vendor}/hub75/src/ESP32-HUB75-MatrixPanel-leddrivers.cpp"
    "${vendor}/hub75/src/platforms/esp32/esp32_i2s_parallel_dma.cpp"
    "${vendor}/wifi-manager/WiFiManager.cpp"
    INCLUDE_DIRS "${vendor}/gfx" "${vendor}/busio" "${vendor}/hub75/src" "${vendor}/wifi-manager"
    REQUIRES espressif__arduino-esp32 esp_wifi esp_netif esp_event espressif__network_provisioning)
# No PIXEL_COLOR_DEPTH_BITS: the colour depth (7 or 8 bit) and its brightness table are chosen at run time, see lib/core/PanelTier.h
