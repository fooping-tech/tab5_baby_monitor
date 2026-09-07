idf_component_register(SRCS "app_main.cpp" "board.cpp" "network.cpp" "live.cpp" "ui.cpp" "jpeg_decoder.cpp"
                           "hal/hal_uvc.cpp" "rtsp/rtsp_server.cpp" "rtsp/rtsp_framing.cpp"
                           "camera_clock_font_64.c" "camera_clock_time_88.c"
                       INCLUDE_DIRS "." "../../core"
                       PRIV_REQUIRES esp_timer usb usb_host_uvc esp_video esp_driver_jpeg
                                     esp_driver_i2c esp_wifi esp_event esp_netif nvs_flash mdns m5stack_tab5)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "This firmware supports ESP32-P4 only")
endif()
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
