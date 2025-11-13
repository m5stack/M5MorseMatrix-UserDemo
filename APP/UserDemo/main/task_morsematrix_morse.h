/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TASK_MORSEMATRIX_MORSE_H
#define TASK_MORSEMATRIX_MORSE_H

#include "m5_morsematrix_component.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 摩尔斯电码任务入口函数
 * @param morsematrix 摩尔斯矩阵组件实例指针
 */
void task_morsematrix_morse_run(m5_morsematrix_component* morsematrix);

#ifdef __cplusplus
}
#endif

#endif // TASK_MORSEMATRIX_MORSE_H
