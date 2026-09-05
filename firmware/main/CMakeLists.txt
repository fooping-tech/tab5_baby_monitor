idf_component_register(SRCS "app_main.cpp" "detector.cpp" "board.cpp" "network.cpp" "live.cpp" "ui.cpp" "jpeg_decoder.cpp"
                           "hal/hal_uvc.cpp" "rtsp/rtsp_server.cpp" "rtsp/rtsp_framing.cpp"
                           "camera_clock_font_64.c" "camera_clock_time_88.c"
                       INCLUDE_DIRS "." "../../core"
                       PRIV_REQUIRES esp-dl yolo26 esp_timer usb usb_host_uvc esp_video esp_driver_jpeg
                                     esp_driver_i2c esp_wifi esp_event esp_netif nvs_flash mdns m5stack_tab5)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    message(FATAL_ERROR "This PoC supports ESP32-P4 only")
endif()
set(POC_JPEG "" CACHE FILEPATH "Explicit local JPEG for still-image smoke test")
if(NOT CONFIG_EDGE_LIVE_CAMERA AND NOT EXISTS "${POC_JPEG}")
    message(FATAL_ERROR "Pass -DPOC_JPEG=/absolute/path/to/consented-test.jpg")
endif()
set(MODEL_FILE "${ESP_DL_PATH}/models/yolo26/models/p4/yolo26n_512_s8_p4.espdl")
file(SHA256 "${MODEL_FILE}" MODEL_SHA256)
if(NOT MODEL_SHA256 STREQUAL "283e4a2ca8626bf34ff4da290597afcc8e5eaa50b939ec589961806e3d9da99a")
    message(FATAL_ERROR "Stock model SHA256 mismatch")
endif()
set(cmake_dir "${ESP_DL_PATH}/esp-dl/fbs_loader/cmake")
include("${cmake_dir}/utilities.cmake")
configure_file("${MODEL_FILE}" "${CMAKE_CURRENT_BINARY_DIR}/person_model.espdl" COPYONLY)
target_add_aligned_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/person_model.espdl" BINARY)
if(NOT CONFIG_EDGE_LIVE_CAMERA)
    configure_file("${POC_JPEG}" "${CMAKE_CURRENT_BINARY_DIR}/poc_image.jpg" COPYONLY)
    target_add_aligned_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_BINARY_DIR}/poc_image.jpg" BINARY)
endif()
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
