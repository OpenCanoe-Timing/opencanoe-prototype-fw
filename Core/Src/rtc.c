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
#include "uart.h"
#include "gnss.h"
#include <stdint.h>
#include <stdio.h>
#include "rtc.h"

#define IMPULSE_LOCKOUT_MS 50U

extern TIM_HandleTypeDef htim2;
static uint32_t last_impulse_time[2] = {0};

static const char pps_string[] = "PPS Recieved!\r\n";

GNSS_DateTime_t utc;
int timestamp_length;

/**
 * @brief Initalise the timing system.
 * 
 */
void TIM_Init(void) {
	HAL_TIM_Base_Start(&htim2);
    HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_1);
	HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_2);
	HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_3);
}


/** 
 * @brief Handles timer input-capture events for the timing channels. 
 * 
 * Determines which timing channel generated the capture event, applies 
 * the configured impulse lockout period, and transmits an impulse message 
 * to the computer over the configured UART interface. 
 * 
 * @param htim Pointer to the timer handle that generated the capture event. 
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1)
    {
        UART_Write(COMPUTER_UART, (const uint8_t *)pps_string, sizeof(pps_string)-1);
    }
    char channel_impulse = '\0';
    uint8_t channel_index;

    switch (htim->Channel) {
    case HAL_TIM_ACTIVE_CHANNEL_2:
        channel_impulse = '1';
        channel_index = 1;
        break;

	case HAL_TIM_ACTIVE_CHANNEL_3:
        channel_impulse = '2';
        channel_index = 2;
        break;

    default:
        return;
    }

    uint32_t now = HAL_GetTick();

    if ((now - last_impulse_time[channel_index]) < IMPULSE_LOCKOUT_MS) {
        return;
    }

    last_impulse_time[channel_index] = now;

    char buffer[80];

    if (GNSS_GetLastUTC(&utc))
    {
      timestamp_length = snprintf((char *)buffer, sizeof(buffer),
                                  "%04u-%02u-%02u %02u:%02u:%02u.%03u %c %c\r\n",
                                  utc.date.year, utc.date.month, utc.date.day,
                                  utc.time.hours, utc.time.minutes, utc.time.seconds,
                                  utc.time.milliseconds, utc.fix_valid ? 'A' : 'V', channel_impulse);
    }
    else
    {
      timestamp_length = snprintf((char *)buffer, sizeof(buffer),
                                  "No GNSS timestamp yet\r\n");
    }

    if (timestamp_length > (int)(sizeof(buffer) - 1))
    {
      /* snprintf() reports the length it would have written; clamp to
       * what actually fit in the buffer. */
      timestamp_length = (int)(sizeof(buffer) - 1);
    }

    if (timestamp_length > 0)
    {
      UART_Write(COMPUTER_UART, (const uint8_t * )buffer, (uint16_t)timestamp_length);
    }
}