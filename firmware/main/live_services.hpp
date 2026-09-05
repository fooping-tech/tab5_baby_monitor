#pragma once
#include "esp_err.h"

esp_err_t edge_board_start();
esp_err_t edge_ui_start();
void edge_ui_status(const char *presence);
esp_err_t edge_network_start();
void edge_live_start(const unsigned char *model_data);
