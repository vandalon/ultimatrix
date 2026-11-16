set(vendor "${CMAKE_CURRENT_LIST_DIR}/../../vendor")
if("$ENV{UM_TARGET}" STREQUAL "esp32s3")
    set(panel_bus "${vendor}/hub75/src/platforms/esp32s3/gdma_lcd_parallel16.cpp")
else()
    set(panel_bus "${vendor}/hub75/src/platforms/esp32/esp32_i2s_parallel_dma.cpp")
endif()
idf_component_register(SRCS
    "${vendor}/gfx/Adafruit_GFX.cpp" "${vendor}/gfx/Adafruit_GrayOLED.cpp"
    "${vendor}/gfx/Adafruit_SPITFT.cpp" "${vendor}/gfx/glcdfont.c"
    "${vendor}/busio/Adafruit_BusIO_Register.cpp" "${vendor}/busio/Adafruit_GenericDevice.cpp"
    "${vendor}/busio/Adafruit_I2CDevice.cpp" "${vendor}/busio/Adafruit_SPIDevice.cpp"
    "${vendor}/hub75/src/ESP32-HUB75-MatrixPanel-I2S-DMA.cpp"
    "${vendor}/hub75/src/ESP32-HUB75-MatrixPanel-leddrivers.cpp"
    "${panel_bus}"
    "${vendor}/wifi-manager/WiFiManager.cpp"
    INCLUDE_DIRS "${vendor}/gfx" "${vendor}/busio" "${vendor}/hub75/src" "${vendor}/wifi-manager"
    REQUIRES espressif__arduino-esp32 esp_wifi esp_netif esp_event espressif__network_provisioning)
# The ESP32-S3 panel backend (LCD_CAM + GDMA) uses the esp_lcd driver.
if("$ENV{UM_TARGET}" STREQUAL "esp32s3")
    idf_component_get_property(_lib ${COMPONENT_NAME} COMPONENT_LIB)
    target_link_libraries(${_lib} PUBLIC idf::esp_lcd idf::esp_driver_gpio)
endif()
# No PIXEL_COLOR_DEPTH_BITS: the colour depth (7 or 8 bit) and its brightness table are chosen at run time, see lib/core/PanelTier.h
