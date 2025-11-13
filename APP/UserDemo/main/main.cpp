/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "esp_err.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"

#include "m5_morsematrix_component.h"

// 摩尔斯电码输入任务
#include "task_morsematrix_morse.h"

static const char *TAG = "main_app";
static const char* VERSION = "P399-MorseMatrix-ESP32C3-PCBV0.2-UserDemo-VF1.2";

// 全局组件实例
m5_morsematrix_component morsematrix;

// 任务入口函数（在独立线程中运行）
static void main_task_entry(void* param) {
    (void)param;
    
    // 摩尔斯电码输入任务
    morsematrix.setup_ir_for_morse_task();
    task_morsematrix_morse_run(&morsematrix);

    // 任务不应该退出，如果退出则删除自己
    ESP_LOGE(TAG, "Main task exited unexpectedly!");
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "VERSION: %s", VERSION);
    ESP_LOGI(TAG, "Starting MorseMatrix initialization...");
    
    // 初始化组件
    esp_err_t init_result = morsematrix.init();
    if (init_result != ESP_OK) {
        ESP_LOGE(TAG, "MorseMatrix initialization FAILED with error: %d", init_result);
        return;
    }
    ESP_LOGI(TAG, "MorseMatrix initialization completed successfully");

    // 安装USB Serial JTAG驱动
    static bool usb_driver_installed = false;
    if (!usb_driver_installed) {
        usb_serial_jtag_driver_config_t usb_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        usb_cfg.rx_buffer_size = 256;
        esp_err_t usb_res = usb_serial_jtag_driver_install(&usb_cfg);
        if (usb_res != ESP_OK && usb_res != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Failed to install USB Serial JTAG driver: %s", esp_err_to_name(usb_res));
        } else {
            usb_driver_installed = true;
        }
    }
    
    // 创建主任务线程
    ESP_LOGI(TAG, "Creating main task thread...");
    BaseType_t result = xTaskCreate(
        main_task_entry,
        "main_task",
        8192,  // 栈大小
        NULL,
        2,     // 优先级 2 - main任务（高于LED刷新，确保初始化完成）
        NULL
    );
    
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create main task thread");
        return;
    }
    
    ESP_LOGI(TAG, "Main task thread created successfully");
    
    // app_main 可以退出，主任务在独立线程中运行
    ESP_LOGI(TAG, "app_main completed, main task is running in separate thread");
}
