/**
 * @file rtc.c
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief Timebase for the application.
 * 
 * @copyright
 * Copyright (c) 2026 Alexander Ellul.
 * 
 * SPDX-License-Identifier: GPL-3.0-only
 * 
 * This file is part of the OpenCanoe Timing System prototype firmware.
 * 
 * This software is licensed under the GNU General Public License v3.0.
 * See the LICENSE.md file in the root directory of this project for details.
 * 
 * This software is provided "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * either expressed or implied.
 * 
 */

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_tim.h"
#include "rtc.h"

extern TIM_HandleTypeDef htim2;

void initalise_timer(void) {
	HAL_TIM_Base_Start(&htim2);
}