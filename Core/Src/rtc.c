/**
 * @file rtc.c
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief High-resolution PPS-disciplined application timebase.
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

#include "gnss.h"
#include "lcd.h"
#include "rtc.h"
#include "uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define IMPULSE_LOCKOUT_MS 50U

#define TIM2_TICKS_PER_100US 10U

extern TIM_HandleTypeDef htim2;

static uint32_t last_impulse_time[2] = {0U, 0U};

/* --------------------------------------------------------------------------
 * Utility
 * -------------------------------------------------------------------------- */

static uint8_t RTC_Uint64ToString(uint64_t value, char *buffer,
                                  uint8_t buffer_size) {
  char temporary[20];

  uint8_t length = 0U;

  do {
    if (length >= sizeof(temporary)) {
      return 0U;
    }

    temporary[length++] = (char)('0' + (value % 10ULL));

    value /= 10ULL;

  } while (value != 0ULL);

  if ((uint8_t)(length + 1U) > buffer_size) {
    return 0U;
  }

  for (uint8_t i = 0U; i < length; i++) {
    buffer[i] = temporary[length - 1U - i];
  }

  buffer[length] = '\0';

  return length;
}

/* --------------------------------------------------------------------------
 * Timer initialisation
 * -------------------------------------------------------------------------- */

void TIM_Init(void) {
  HAL_TIM_Base_Start(&htim2);

  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_1);

  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_2);

  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_3);
}

/* --------------------------------------------------------------------------
 * Input capture
 * -------------------------------------------------------------------------- */

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
  if (htim->Instance != TIM2) {
    return;
  }

  /* ============================================================
   * GNSS PPS
   * ============================================================
   */

  if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
    uint32_t pps_tick = HAL_GetTick();

    /*
     * TIM2 is reset by the hardware PPS trigger.
     *
     * GNSS_ProcessPPS() establishes the UTC second
     * represented by this physical PPS.
     */
    (void)GNSS_ProcessPPS(pps_tick);

    return;
  }

  /* ============================================================
   * Timing inputs
   * ============================================================
   */

  char channel_impulse = '\0';

  uint8_t channel_index = 0U;

  switch (htim->Channel) {
  case HAL_TIM_ACTIVE_CHANNEL_2:

    channel_impulse = '1';
    channel_index = 0U;

    break;

  case HAL_TIM_ACTIVE_CHANNEL_3:

    channel_impulse = '2';
    channel_index = 1U;

    break;

  default:

    return;
  }

  /* ============================================================
   * Input lockout
   * ============================================================
   */

  uint32_t now = HAL_GetTick();

  /*
   * Unsigned subtraction intentionally handles HAL_GetTick()
   * rollover correctly.
   */
  if ((uint32_t)(now - last_impulse_time[channel_index]) < IMPULSE_LOCKOUT_MS) {
    return;
  }

  last_impulse_time[channel_index] = now;

  /* ============================================================
   * Require PPS lock
   * ============================================================
   */

  uint64_t current_unix_100us;

  if (!GNSS_GetLastUnix100us(&current_unix_100us)) {
    return;
  }

  /* ============================================================
   * Calculate timestamp
   * ============================================================
   */

  uint32_t timer_ticks = __HAL_TIM_GET_COUNTER(&htim2);

  uint64_t timestamp_100us =
      current_unix_100us + ((uint64_t)timer_ticks / TIM2_TICKS_PER_100US);

  /* ============================================================
   * Notify LCD
   * ============================================================
   */

  LCD_RequestImpulse(channel_impulse);

  /* ============================================================
   * Convert timestamp
   * ============================================================
   */

  char timestamp_string[24];

  uint8_t timestamp_length = RTC_Uint64ToString(
      timestamp_100us, timestamp_string, sizeof(timestamp_string));

  if (timestamp_length == 0U) {
    return;
  }

  /* ============================================================
   * Build output
   *
   *     <timestamp>,<channel>\r\n
   * ============================================================
   */

  char buffer[32];

  uint8_t position = 0U;

  memcpy(&buffer[position], timestamp_string, timestamp_length);

  position += timestamp_length;

  buffer[position++] = ',';

  buffer[position++] = channel_impulse;

  buffer[position++] = '\r';
  buffer[position++] = '\n';

  /* ============================================================
   * Transmit
   * ============================================================
   */

  UART_Write(COMPUTER_UART, (const uint8_t *)buffer, position);
}