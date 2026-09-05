#include "live_services.hpp"
#include "driver/i2c_master.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace {
void usb_events(void *)
{
    while (true) {
        uint32_t flags = 0;
        if (usb_host_lib_handle_events(portMAX_DELAY, &flags) == ESP_OK &&
            (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)) usb_host_device_free_all();
    }
}
}

esp_err_t edge_board_start()
{
    i2c_master_bus_handle_t bus = nullptr;
    i2c_master_bus_config_t config{};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = GPIO_NUM_31;
    config.scl_io_num = GPIO_NUM_32;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.flags.enable_internal_pullup = true;
    ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = 0x44;
    device_config.scl_speed_hz = 400000;
    i2c_master_dev_handle_t device = nullptr;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &device_config, &device));
    const uint8_t registers[][2] = {
        {0x01, 0xff}, {0x03, 0xb9}, {0x07, 0x06}, {0x0d, 0xb9},
        {0x0b, 0xf9}, {0x09, 0x40}, {0x11, 0xbf}, {0x05, 0x09},
    };
    for (const auto &entry : registers) ESP_ERROR_CHECK(i2c_master_transmit(device, entry, 2, 1000));
    vTaskDelay(pdMS_TO_TICKS(100));
    usb_host_config_t usb{};
    usb.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&usb));
    if (xTaskCreate(usb_events, "usb_lib", 4096, nullptr, 10, nullptr) != pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
