/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __M5_MORSEMATRIX_WIFI_H__
#define __M5_MORSEMATRIX_WIFI_H__

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

class m5_morsematrix_component;

/**
 * @brief 摩尔斯矩阵 WiFi 管理类
 * 提供 WiFi 连接和状态管理功能
 */
class m5_morsematrix_wifi {
public:
    /**
     * @brief 构造函数
     */
    m5_morsematrix_wifi();

    /**
     * @brief 析构函数
     */
    ~m5_morsematrix_wifi();

    /**
     * @brief 设置所属的组件
     * @param owner 组件指针
     */
    void set_owner(m5_morsematrix_component* owner);

    /**
     * @brief 初始化并连接到指定的AP
     * @param ssid WiFi SSID
     * @param password WiFi 密码
     * @return esp_err_t 初始化结果
     */
    esp_err_t init_and_connect(const char* ssid, const char* password);

    /**
     * @brief 断开 WiFi 连接
     * @return esp_err_t 操作结果
     */
    esp_err_t disconnect();

    /**
     * @brief 获取 WiFi 连接状态
     * @return true=已连接, false=未连接
     */
    bool is_connected() const { return connected_; }

    /**
     * @brief 获取是否已初始化
     * @return true=已初始化, false=未初始化
     */
    bool is_initialized() const { return initialized_; }

    /**
     * @brief 获取所属的组件
     * @return 组件指针
     */
    m5_morsematrix_component* owner() const { return owner_; }

private:
    /**
     * @brief WiFi 事件处理函数
     */
    static void event_handler(void* arg, esp_event_base_t event_base, 
                             int32_t event_id, void* event_data);

    m5_morsematrix_component* owner_ = nullptr;
    EventGroupHandle_t event_group_ = nullptr;
    bool initialized_ = false;
    bool connected_ = false;
};

#endif // __M5_MORSEMATRIX_WIFI_H__
