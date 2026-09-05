#include "live_services.hpp"
#include <atomic>
#include <cassert>
#include <cstring>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "esp_wifi_netif.h"
#include "esp_private/wifi.h"
#include "esp_log.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtsp/rtsp_server.h"

namespace {
constexpr char TAG[] = "network";
static bool s_wifi_sta_netif_started = false;
std::atomic<bool> connecting{false};
bool mdns_started = false;
int retries = 0;
static void wifi_remote_sta_start_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (s_wifi_sta_netif_started || esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi STA start event");
        return;
    }

    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    uint8_t mac[6];
    esp_err_t ret = esp_wifi_get_if_mac(driver, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_if_mac failed: %s", esp_err_to_name(ret));
        return;
    }

    if (esp_wifi_is_if_ready_when_started(driver)) {
        ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netstack cb register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, event_id, data);
    s_wifi_sta_netif_started = true;
}

static void wifi_remote_sta_stop_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (!s_wifi_sta_netif_started && !esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi STA stop event");
        return;
    }

    esp_netif_action_stop(netif, base, event_id, data);
    s_wifi_sta_netif_started = false;
}

static void wifi_remote_sta_connected_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));

    // A station interface is not ready at STA_START. The ESP-IDF default
    // handler registers its RX callback at STA_CONNECTED; the custom
    // Hosted-netif start handler above must do the same or DHCP never sees
    // packets from the ESP32-C6.
    if (!esp_wifi_is_if_ready_when_started(driver)) {
        const esp_err_t ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb on STA_CONNECTED failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    esp_netif_action_connected(netif, base, event_id, data);
    ESP_LOGI(TAG, "Wi-Fi STA connected; network interface is up");
}

static esp_netif_t* create_wifi_remote_sta_netif()
{
    // Tab5's ESP32-C6 Wi-Fi is hosted by the P4. Preserve the official
    // UserDemo's explicit remote-netif wiring, but attach the STA interface
    // so the camera joins the existing LAN instead of creating a demo AP.
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t* netif     = esp_netif_new(&cfg);
    assert(netif);

    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_remote_sta_start_handler, netif));
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_remote_sta_connected_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, wifi_remote_sta_stop_handler, netif));

    return netif;
}


void events(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!connecting.exchange(true)) esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_netif_action_disconnected(arg, base, event_id, data);
        if (++retries <= CONFIG_TAB5_WIFI_MAX_RETRY) esp_wifi_connect();
        else ESP_LOGE(TAG, "Wi-Fi retries exhausted; reboot or reconfigure network");
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        retries = 0;
        if (!mdns_started && mdns_init() == ESP_OK) {
            mdns_hostname_set(CONFIG_EDGE_HOSTNAME);
            mdns_service_add("Tab5 Edge AI", "_rtsp", "_tcp", 8554, nullptr, 0);
            mdns_started = true;
        }
        const auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "RTSP LAN address: rtsp://" IPSTR ":8554/baby", IP2STR(&event->ip_info.ip));
        ESP_ERROR_CHECK(rtsp_server_start());
    }
}
}

esp_err_t edge_network_start()
{
#if !CONFIG_EDGE_ENABLE_RTSP
    ESP_LOGW(TAG, "RTSP disabled; local inference only");
    return ESP_OK;
#endif
    if (CONFIG_TAB5_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "RTSP disabled or SSID empty; local inference only");
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    auto *netif = create_wifi_remote_sta_netif();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, events, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, events, netif));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t config{};
    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), CONFIG_TAB5_WIFI_SSID, sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(config.sta.password), CONFIG_TAB5_WIFI_PASSWORD, sizeof(config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!connecting.exchange(true)) return esp_wifi_connect();
    return ESP_OK;
}
