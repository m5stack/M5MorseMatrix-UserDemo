/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "m5_morsematrix_component.h"

// LED映射表定义
// 对角线位置没有LED(阳极和阴极短路)
const led_map_t m5_morsematrix_component::led_map[8][8] = {
    // Row 0 (y=0)
    {{GPIO_NUM_1, GPIO_NUM_0}, {GPIO_NUM_2, GPIO_NUM_0}, {GPIO_NUM_3, GPIO_NUM_0}, {GPIO_NUM_4, GPIO_NUM_0}, {GPIO_NUM_5, GPIO_NUM_0}, {GPIO_NUM_6, GPIO_NUM_0}, {GPIO_NUM_7, GPIO_NUM_0}, {GPIO_NUM_0, GPIO_NUM_1}},
    
    // Row 1 (y=1)
    {{GPIO_NUM_2, GPIO_NUM_1}, {GPIO_NUM_3, GPIO_NUM_1}, {GPIO_NUM_4, GPIO_NUM_1}, {GPIO_NUM_5, GPIO_NUM_1}, {GPIO_NUM_6, GPIO_NUM_1}, {GPIO_NUM_7, GPIO_NUM_1}, {GPIO_NUM_0, GPIO_NUM_2}, {GPIO_NUM_1, GPIO_NUM_2}},
    
    // Row 2 (y=2)
    {{GPIO_NUM_3, GPIO_NUM_2}, {GPIO_NUM_4, GPIO_NUM_2}, {GPIO_NUM_5, GPIO_NUM_2}, {GPIO_NUM_6, GPIO_NUM_2}, {GPIO_NUM_7, GPIO_NUM_2}, {GPIO_NUM_0, GPIO_NUM_3}, {GPIO_NUM_1, GPIO_NUM_3}, {GPIO_NUM_2, GPIO_NUM_3}},
    
    // Row 3 (y=3)
    {{GPIO_NUM_4, GPIO_NUM_3}, {GPIO_NUM_5, GPIO_NUM_3}, {GPIO_NUM_6, GPIO_NUM_3}, {GPIO_NUM_7, GPIO_NUM_3}, {GPIO_NUM_0, GPIO_NUM_4}, {GPIO_NUM_1, GPIO_NUM_4}, {GPIO_NUM_2, GPIO_NUM_4}, {GPIO_NUM_3, GPIO_NUM_4}},
    
    // Row 4 (y=4)
    {{GPIO_NUM_5, GPIO_NUM_4}, {GPIO_NUM_6, GPIO_NUM_4}, {GPIO_NUM_7, GPIO_NUM_4}, {GPIO_NUM_0, GPIO_NUM_5}, {GPIO_NUM_1, GPIO_NUM_5}, {GPIO_NUM_2, GPIO_NUM_5}, {GPIO_NUM_3, GPIO_NUM_5}, {GPIO_NUM_4, GPIO_NUM_5}},
    
    // Row 5 (y=5)
#ifdef MORSEMATRIX_USE_PCB_V0_1
    {{GPIO_NUM_6, GPIO_NUM_5}, {GPIO_NUM_7, GPIO_NUM_5}, {GPIO_NUM_0, GPIO_NUM_6}, {GPIO_NUM_1, GPIO_NUM_6}, {GPIO_NUM_4, GPIO_NUM_6}, {GPIO_NUM_5, GPIO_NUM_6}, {GPIO_NUM_2, GPIO_NUM_6}, {GPIO_NUM_3, GPIO_NUM_6}},
#else
    {{GPIO_NUM_6, GPIO_NUM_5}, {GPIO_NUM_7, GPIO_NUM_5}, {GPIO_NUM_0, GPIO_NUM_6}, {GPIO_NUM_1, GPIO_NUM_6}, {GPIO_NUM_2, GPIO_NUM_6}, {GPIO_NUM_3, GPIO_NUM_6}, {GPIO_NUM_4, GPIO_NUM_6}, {GPIO_NUM_5, GPIO_NUM_6}},
#endif
    
    // Row 6 (y=6)
    {{GPIO_NUM_7, GPIO_NUM_6}, {GPIO_NUM_0, GPIO_NUM_7}, {GPIO_NUM_1, GPIO_NUM_7}, {GPIO_NUM_2, GPIO_NUM_7}, {GPIO_NUM_3, GPIO_NUM_7}, {GPIO_NUM_4, GPIO_NUM_7}, {GPIO_NUM_5, GPIO_NUM_7}, {GPIO_NUM_6, GPIO_NUM_7}},
    
    // Row 7 (y=7)
    {{GPIO_NUM_0, GPIO_NUM_10}, {GPIO_NUM_1, GPIO_NUM_10}, {GPIO_NUM_2, GPIO_NUM_10}, {GPIO_NUM_3, GPIO_NUM_10}, {GPIO_NUM_4, GPIO_NUM_10}, {GPIO_NUM_5, GPIO_NUM_10}, {GPIO_NUM_6, GPIO_NUM_10}, {GPIO_NUM_7, GPIO_NUM_10}}
};

const m5_morsematrix_component::brightness_level_t m5_morsematrix_component::kBrightnessLevels[m5_morsematrix_component::kBrightnessLevelCount] = {
    {  0,  1000},  // Level 0: 关闭 (0% 占空比)
    {  20,  980},  // Level 1: 最暗可见 (2% 占空比) - 保持不变
    {  65,  935},  // Level 2: 很暗 (6.5% 占空比) - 保持不变
    { 120,  880},  // Level 3: 较暗 (12% 占空比) - 按比例放大
    { 198,  802},  // Level 4: 中等偏暗 (19.8% 占空比) - 按比例放大
    { 276,  724},  // Level 5: 中等 (27.6% 占空比) - 按比例放大
    { 432,  568},  // Level 6: 较亮 (43.2% 占空比) - 按比例放大
    { 900,  100}   // Level 7: 最亮 (90% 占空比) - 目标值
};
