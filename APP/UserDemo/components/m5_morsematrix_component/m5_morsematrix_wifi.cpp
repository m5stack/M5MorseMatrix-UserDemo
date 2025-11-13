/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_morsematrix_wifi.h"
#include "m5_morsematrix_component.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char* TAG = "M5_WIFI";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

m5_morsematrix_wifi::m5_morsematrix_wifi()
{
}

m5_morsematrix_wifi::~m5_morsematrix_wifi()
{
    if (initialized_) {
        disconnect();
    }
    if (event_group_) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

void m5_morsematrix_wifi::set_owner(m5_morsematrix_component* owner)
{
    owner_ = owner;
}

esp_err_t m5_morsematrix_wifi::init_and_connect(const char* ssid, const char* password)
{
    if (initialized_) {
        ESP_LOGW(TAG, "WiFi already initialized");
        return ESP_OK;
    }

    // 创建事件组
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化 TCP/IP 栈
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // WiFi 初始化配置
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                &event_handler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, 
                                                &event_handler, this));

    // WiFi 配置
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished");

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(event_group_,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    initialized_ = true;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);
        connected_ = true;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s", ssid);
        connected_ = false;
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "Unexpected event");
        return ESP_FAIL;
    }
}

esp_err_t m5_morsematrix_wifi::disconnect()
{
    if (!initialized_) {
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_stop();
    if (ret == ESP_OK) {
        connected_ = false;
        ESP_LOGI(TAG, "WiFi disconnected");
    }
    return ret;
}

void m5_morsematrix_wifi::event_handler(void* arg, esp_event_base_t event_base,
                                        int32_t event_id, void* event_data)
{
    m5_morsematrix_wifi* self = static_cast<m5_morsematrix_wifi*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "WiFi station started, connecting...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        self->connected_ = false;
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        self->connected_ = true;
        if (self->event_group_) {
            xEventGroupSetBits(self->event_group_, WIFI_CONNECTED_BIT);
        }
    }
}
