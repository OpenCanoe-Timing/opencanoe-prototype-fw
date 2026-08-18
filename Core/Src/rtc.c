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
#include "rtc.h"
#include "uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * Timing input lockout.
 *
 * This prevents a single physical input producing multiple
 * timing events.
 */
#define IMPULSE_LOCKOUT_MS 50U

/*
 * TIM2:
 *
 *     1 tick = 10 us
 *
 * Therefore:
 *
 *     10 ticks = 100 us
 */
#define TIM2_TICKS_PER_100US 10U

/*
 * ============================================================
 * Timer state
 * ============================================================
 */

extern TIM_HandleTypeDef htim2;

/*
 * Last HAL tick at which each timing channel generated an
 * accepted impulse.
 *
 * Index 0 = channel 2
 * Index 1 = channel 3
 */
static uint32_t last_impulse_time[2] = {0U};

/*
 * ============================================================
 * Utility functions
 * ============================================================
 */

/**
 * @brief Convert uint64_t to decimal ASCII.
 *
 * This avoids depending on %llu support in the embedded
 * printf implementation.
 */
static uint8_t RTC_Uint64ToString(uint64_t value, char *buffer,
                                  uint8_t buffer_size) {
  char temporary[20];
  uint8_t length = 0U;

  /*
   * uint64_t requires at most 20 decimal digits.
   */
  do {
    if (length >= sizeof(temporary)) {
      return 0U;
    }

    temporary[length++] = (char)('0' + (value % 10ULL));

    value /= 10ULL;

  } while (value != 0ULL);

  /*
   * Need room for null terminator.
   */
  if ((uint8_t)(length + 1U) > buffer_size) {
    return 0U;
  }

  /*
   * Reverse the digits.
   */
  for (uint8_t i = 0U; i < length; i++) {
    buffer[i] = temporary[length - 1U - i];
  }

  buffer[length] = '\0';

  return length;
}

/*
 * ============================================================
 * Timer initialisation
 * ============================================================
 */

/**
 * @brief Initialise the high-resolution timing timer.
 *
 * TIM2 is configured by CubeMX.
 *
 * Expected configuration:
 *
 *     Prescaler -> produces 10 us timer ticks
 *     Period    -> 0xFFFFFFFF
 *
 * TIM2 Channel 1:
 *
 *     GNSS PPS
 *
 * TIM2 Channel 2:
 *
 *     Timing input 1
 *
 * TIM2 Channel 3:
 *
 *     Timing input 2
 *
 * The GNSS PPS input is also configured in CubeMX as the
 * timer slave reset trigger.
 *
 * Therefore the hardware resets TIM2 to zero at every PPS.
 */
void TIM_Init(void) {
  /*
   * Start the timer base.
   */
  HAL_TIM_Base_Start(&htim2);

  /*
   * GNSS PPS.
   */
  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_1);

  /*
   * Timing input 1.
   */
  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_2);

  /*
   * Timing input 2.
   */
  HAL_TIM_IC_Start_IT(&htim2, TIM_CHANNEL_3);
}

/*
 * ============================================================
 * Input capture callback
 * ============================================================
 */

/**
 * @brief Handle TIM2 input capture events.
 *
 * Channel assignment:
 *
 *     CH1 = GNSS PPS
 *     CH2 = timing input 1
 *     CH3 = timing input 2
 *
 * The PPS is the UTC second boundary.
 *
 * Because TIM2 is configured in hardware slave reset mode,
 * TIM2->CNT is reset to zero by the PPS hardware trigger.
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim) {
  /*
   * Only process TIM2.
   */
  if (htim->Instance != TIM2) {
    return;
  }

  /*
   * ========================================================
   * GNSS PPS
   * ========================================================
   */
  if (htim->Channel == HAL_TIM_ACTIVE_CHANNEL_1) {
    /*
     * Capture the MCU tick as close to the PPS interrupt
     * as possible.
     *
     * GNSS_ProcessPPS() uses this to associate the latest
     * RMC sentence with this physical PPS.
     */
    uint32_t pps_tick = HAL_GetTick();

    /*
     * TIM2 has already been reset by the hardware slave
     * reset trigger.
     *
     * GNSS now determines which UTC second this physical
     * PPS represents.
     *
     * GNSS_ProcessPPS() also requests the LCD update.
     */
    (void)GNSS_ProcessPPS(pps_tick);

    return;
  }

  /*
   * ========================================================
   * Timing channels
   * ========================================================
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

  /*
   * ========================================================
   * Input lockout
   * ========================================================
   */

  uint32_t now = HAL_GetTick();

  if ((now - last_impulse_time[channel_index]) < IMPULSE_LOCKOUT_MS) {
    return;
  }

  last_impulse_time[channel_index] = now;

  /*
   * ========================================================
   * Check GNSS lock
   * ========================================================
   *
   * Do not produce UTC timestamps until at least one PPS
   * has established the UTC second.
   */
  uint64_t current_unix_100us;

  if (!GNSS_GetLastUnix100us(&current_unix_100us)) {
    return;
  }

  /*
   * ========================================================
   * Read TIM2
   * ========================================================
   *
   * TIM2 was reset at the beginning of the UTC second.
   *
   * Therefore:
   *
   *     TIM2 CNT = elapsed time since PPS
   *
   * and:
   *
   *     CNT / 10 = elapsed time in 100 us units.
   */
  uint32_t timer_ticks = __HAL_TIM_GET_COUNTER(&htim2);

  uint64_t timestamp_100us =
      current_unix_100us + ((uint64_t)timer_ticks / TIM2_TICKS_PER_100US);

  /*
   * ========================================================
   * Convert timestamp to ASCII
   * ========================================================
   */

  char timestamp_string[24];

  uint8_t timestamp_length = RTC_Uint64ToString(
      timestamp_100us, timestamp_string, sizeof(timestamp_string));

  if (timestamp_length == 0U) {
    return;
  }

  /*
   * ========================================================
   * Build output packet
   * ========================================================
   *
   *     <timestamp>,<channel>\r\n
   *
   * Example:
   *
   *     17868723451234,1
   */
  char buffer[32];

  uint8_t position = 0U;

  /*
   * Timestamp.
   */
  memcpy(&buffer[position], timestamp_string, timestamp_length);

  position += timestamp_length;

  /*
   * Separator.
   */
  buffer[position++] = ',';

  /*
   * Channel.
   */
  buffer[position++] = channel_impulse;

  /*
   * CR/LF.
   */
  buffer[position++] = '\r';
  buffer[position++] = '\n';

  /*
   * ========================================================
   * Transmit
   * ========================================================
   */
  UART_Write(COMPUTER_UART, (const uint8_t *)buffer, position);
}