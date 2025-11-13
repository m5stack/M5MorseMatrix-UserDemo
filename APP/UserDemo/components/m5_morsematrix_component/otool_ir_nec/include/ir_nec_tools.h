/*
 * SPDX-FileCopyrightText: 2025 exia
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef __IR_NEC_TOOLS_H__
#define __IR_NEC_TOOLS_H__

#include "esp_err.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// 默认引脚和参数，可在实例化后修改
#define IR_NEC_DEFAULT_RESOLUTION_HZ   1000000
#define IR_NEC_DEFAULT_TX_GPIO         (gpio_num_t)-1
#define IR_NEC_DEFAULT_RX_GPIO         (gpio_num_t)-1
#define IR_NEC_DEFAULT_EN_GPIO         (gpio_num_t)-1

// NEC 解码相关常量
#define IR_NEC_DECODE_MARGIN           200    // 解码容差，单位us
#define NEC_LEADING_CODE_DURATION_0    9000
#define NEC_LEADING_CODE_DURATION_1    4500
#define NEC_PAYLOAD_ZERO_DURATION_0    560
#define NEC_PAYLOAD_ZERO_DURATION_1    560
#define NEC_PAYLOAD_ONE_DURATION_0     560
#define NEC_PAYLOAD_ONE_DURATION_1     1690
#define NEC_REPEAT_CODE_DURATION_0     9000
#define NEC_REPEAT_CODE_DURATION_1     2250

// 回调函数类型定义
typedef void (*ir_nec_receive_callback_t)(uint16_t address, uint16_t command, bool is_repeat);
typedef void (*ir_nec_enable_callback_t)(bool enable); // EN控制回调函数类型

/**
 * @brief 简单的NEC红外收发工具类
 */
class ir_nec_tools {
private:
    bool initialized = false;
    uint32_t resolution_hz = IR_NEC_DEFAULT_RESOLUTION_HZ;
    gpio_num_t tx_gpio = IR_NEC_DEFAULT_TX_GPIO;
    gpio_num_t rx_gpio = IR_NEC_DEFAULT_RX_GPIO; 
    gpio_num_t en_gpio = IR_NEC_DEFAULT_EN_GPIO;

    rmt_channel_handle_t tx_channel = nullptr;
    rmt_channel_handle_t rx_channel = nullptr;
    rmt_encoder_handle_t nec_encoder = nullptr;
    
    QueueHandle_t receive_queue = nullptr;
    TaskHandle_t rx_task_handle = nullptr;
    ir_nec_receive_callback_t receive_callback = nullptr;
    ir_nec_enable_callback_t enable_callback = nullptr; // EN控制回调函数
    volatile bool rx_task_should_exit = false;
    
    // RX相关成员变量
    uint16_t last_address = 0;
    uint16_t last_command = 0;
    uint32_t nec_packet_counter = 0;  // 全局NEC包计数器
    
    // 私有方法
    esp_err_t init_tx_channel();
    esp_err_t init_rx_channel();
    static bool rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data);
    static void rx_task(void *pvParameters);
    bool parse_nec_frame(rmt_symbol_word_t *symbols, size_t symbol_num);
    bool check_in_range(uint32_t signal_duration, uint32_t spec_duration);
    bool parse_logic0(rmt_symbol_word_t *symbol);
    bool parse_logic1(rmt_symbol_word_t *symbol);
    bool parse_repeat_frame(rmt_symbol_word_t *symbol);

public:
    ir_nec_tools() = default;
    ~ir_nec_tools();

    /**
     * @brief 初始化发送通道和编码器（使用默认配置）
     */
    esp_err_t init();

    /**
     * @brief 初始化发送通道和编码器（自定义GPIO配置）
     * @param tx_gpio_pin 发送引脚（可传-1）
     * @param rx_gpio_pin 接收引脚（可传-1）
     * @param en_gpio_pin 使能引脚（可传-1）
     * @note TX和RX不能同时为-1，至少需要配置一个
     */
    esp_err_t init(gpio_num_t tx_gpio_pin, gpio_num_t rx_gpio_pin, gpio_num_t en_gpio_pin);

    /**
     * @brief 初始化发送通道和编码器（自定义GPIO配置，使用回调函数控制EN）
     * @param tx_gpio_pin 发送引脚（可传-1）
     * @param rx_gpio_pin 接收引脚（可传-1）
     * @param enable_cb EN控制回调函数（可传nullptr）
     * @note TX和RX不能同时为-1，至少需要配置一个
     */
    esp_err_t init(gpio_num_t tx_gpio_pin, gpio_num_t rx_gpio_pin, ir_nec_enable_callback_t enable_cb);

    /**
     * @brief 反初始化
     */
    esp_err_t deinit();

    /**
     * @brief 发送一个 NEC 扫描码
     */
    esp_err_t send(uint16_t address, uint16_t command);

    /**
     * @brief 设置接收回调函数
     */
    void set_receive_callback(ir_nec_receive_callback_t callback);

    /**
     * @brief 设置EN控制回调函数
     */
    void set_enable_callback(ir_nec_enable_callback_t callback);

    /**
     * @brief 启动接收功能
     */
    esp_err_t start_receive();

    /**
     * @brief 停止接收功能
     */
    esp_err_t stop_receive();

    bool is_initialized() const { return initialized; }

    void set_resolution(uint32_t hz) { resolution_hz = hz; }
    void set_tx_gpio(gpio_num_t gpio) { tx_gpio = gpio; }
    void set_rx_gpio(gpio_num_t gpio) { rx_gpio = gpio; }
};

#endif // __IR_NEC_TOOLS_H__
