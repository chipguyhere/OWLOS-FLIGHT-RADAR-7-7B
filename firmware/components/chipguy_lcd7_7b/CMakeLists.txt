# chipguyhere's Waveshare ESP32-S3-Touch-LCD-7 / -7B board and display libraries
# (chipguy_Waveshare-ESP32-S3-Touch-LCD-7-7B_board and _display, 0.1.0), vendored, with the
# Arduino calls they make supplied by shim/. Compiled only for the Waveshare 7/7B (core/config.h 15,
# 16): for every other board the component is there but empty. the requirements are listed either
# way, ESP-IDF works them out before it knows which board this is
set(SRCS "")
set(INCS "")
if(RADAR_DISPLAY EQUAL 15 OR RADAR_DISPLAY EQUAL 16)
    set(SRCS "ESP32S3_Touch_LCD_7_7B_Board.cpp" "chipguy_ThreadsafeWire.cpp"
             "chipguy_ESP32S3_Touch_LCD_7_7B_display.cpp" "shim/arduino_shim.cpp")
    set(INCS "." "shim")
endif()
idf_component_register(SRCS ${SRCS}
                       INCLUDE_DIRS ${INCS}
                       REQUIRES esp_lcd esp_driver_gpio esp_driver_gptimer esp_driver_i2c esp_timer
                                esp_mm esp_hw_support esp_rom freertos log heap)
if(SRCS)
    # 10-row bounce buffers instead of the library's 20: 20 KB each at 1024 wide. the RAM is short on
    # this firmware (WiFi, TLS, the strips), and the moving layer's work per chunk halves with them
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LCD7_7B_BOUNCE_ROWS=10)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function
                           -Wno-missing-field-initializers -Wno-format)
endif()
