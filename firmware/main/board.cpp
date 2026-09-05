#include "live_services.hpp"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp/m5stack_tab5.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

namespace {
constexpr char kTag[] = "usb_probe";
usb_host_client_handle_t client;

void event_callback(const usb_host_client_event_msg_t *event, void *)
{
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        ESP_LOGI(kTag, "USB device enumerated at address %u", static_cast<unsigned>(event->new_dev.address));
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(kTag, "USB device disconnected");
    }
}

void probe_task(void *)
{
    while (true) {
        usb_host_client_handle_events(client, portMAX_DELAY);
    }
}
}

esp_err_t edge_board_start()
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), "board", "I2C initialization failed");
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    ESP_RETURN_ON_ERROR(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true), "board", "USB host failed");
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async = {.client_event_callback = event_callback, .callback_arg = nullptr},
    };
    ESP_RETURN_ON_ERROR(usb_host_client_register(&config, &client), "board", "USB probe registration failed");
    return xTaskCreate(probe_task, "usb_probe", 4096, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
