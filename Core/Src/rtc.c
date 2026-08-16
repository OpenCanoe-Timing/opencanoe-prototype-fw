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
#include "rtc.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define IMPULSE_LOCKOUT_MS 50U

/*
 * TIM2 runs at 10 us per tick.
 *
 * Therefore:
 *
 *     10 TIM2 ticks = 100 us
 */
#define TIM2_TICKS_PER_100US 10U

/*
 * There are 10,000 x 100 us intervals in one second.
 */
#define UNIX_100US_PER_SECOND 10000ULL

extern TIM_HandleTypeDef htim2;

/*
 * Last HAL tick at which each timing channel generated
 * an accepted impulse.
 *
 * Index 0 = channel 1
 * Index 1 = channel 2
 */
static uint32_t last_impulse_time[2] = {0};

/*
 * Unix timestamp at the beginning of the current UTC second,
 * represented in 100 us units.
 *
 * TIM2 is reset by the GNSS PPS, so TIM2->CNT provides the
 * sub-second offset from this timestamp.
 */
static volatile uint64_t current_unix_100us = 0;

/*
 * True once GNSS + PPS have established a valid Unix timestamp.
 */
static volatile bool unix_time_locked = false;


/**
 * @brief Convert a uint64_t to a decimal ASCII string.
 *
 * This is used instead of printf("%llu") because some embedded
 * printf implementations, particularly reduced/newlib-nano
 * configurations, do not support 64-bit integer formatting.
 *
 * @param value Value to convert.
 * @param buffer Destination buffer.
 * @param buffer_size Size of destination buffer.
 *
 * @return Number of characters written, excluding the null terminator.
 *         Returns 0 if the buffer is too small.
 */
static uint8_t RTC_Uint64ToString(uint64_t value,
                                  char *buffer,
                                  uint8_t buffer_size)
{
    char temporary[20];
    uint8_t length = 0;

    /*
     * uint64_t can contain up to 20 decimal digits.
     */
    do
    {
        if (length >= sizeof(temporary))
        {
            return 0;
        }

        temporary[length++] =
            (char)('0' + (value % 10U));

        value /= 10U;

    } while (value != 0U);


    /*
     * Need room for the digits and null terminator.
     */
    if ((uint8_t)(length + 1U) > buffer_size)
    {
        return 0;
    }


    /*
     * Reverse the digits.
     */
    for (uint8_t i = 0; i < length; i++)
    {
        buffer[i] =
            temporary[length - 1U - i];
    }

    buffer[length] = '\0';

    return length;
}


/**
 * @brief Initialise the timing system.
 *
 * TIM2 is used as the high-resolution timing counter.
 *
 * TIM2 configuration:
 *
 *     1 count = 10 us
 *
 * TIM2 is reset by the GNSS PPS signal.
 */
void TIM_Init(void)
{
    HAL_TIM_Base_Start(&htim2);

    HAL_TIM_IC_Start_IT(
        &htim2,
        TIM_CHANNEL_1);

    HAL_TIM_IC_Start_IT(
        &htim2,
        TIM_CHANNEL_2);

    HAL_TIM_IC_Start_IT(
        &htim2,
        TIM_CHANNEL_3);
}


/**
 * @brief Handles timer input-capture events.
 *
 * TIM2 channel assignments:
 *
 *     Channel 1 = GNSS PPS
 *     Channel 2 = timing input 1
 *     Channel 3 = timing input 2
 *
 * The GNSS PPS establishes the beginning of a UTC second.
 *
 * Timing events use the current TIM2 counter value to determine
 * their position within that second.
 *
 * Since TIM2 runs at 10 us/tick:
 *
 *     timestamp_100us =
 *         current_second_100us +
 *         (TIM2_CNT / 10)
 *
 * @param htim Pointer to the timer handle that generated the event.
 */
void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *htim)
{
    /*
     * Only handle TIM2.
     */
    if (htim->Instance != TIM2)
    {
        return;
    }


    /*
     * ============================================================
     * GNSS PPS
     * ============================================================
     *
     * Channel 1 is connected to the GNSS PPS.
     *
     * TIM2 is configured in hardware to reset on this input.
     * Therefore, at the PPS:
     *
     *     TIM2 = 0
     *
     * We use the latest RMC UTC value to establish which UTC
     * second this PPS represents.
     */
    if (htim->Channel ==
        HAL_TIM_ACTIVE_CHANNEL_1)
    {
        GNSS_DateTime_t utc;

        if (GNSS_GetLastUTC(&utc))
        {
            /*
             * Convert the RMC time/date to Unix time in 100 us
             * units.
             */
            uint64_t unix_100us =
                GNSS_DateTimeToUnix100us(&utc);


            /*
             * Remove the fractional second.
             *
             * We only want:
             *
             *     YYYY-MM-DD HH:MM:SS.000000
             */
            unix_100us =
                (unix_100us /
                 UNIX_100US_PER_SECOND) *
                UNIX_100US_PER_SECOND;


            /*
             * The PPS marks the beginning of the next second.
             *
             * For example:
             *
             * RMC = 12:34:56
             * PPS = 12:34:57.000000
             */
            unix_100us +=
                UNIX_100US_PER_SECOND;


            /*
             * Store the beginning of this UTC second.
             */
            current_unix_100us =
                unix_100us;

            unix_time_locked = true;
        }

        return;
    }


    /*
     * ============================================================
     * Timing channels
     * ============================================================
     */

    char channel_impulse = '\0';
    uint8_t channel_index;


    switch (htim->Channel)
    {
        case HAL_TIM_ACTIVE_CHANNEL_2:
            channel_impulse = '1';
            channel_index = 0;
            break;

        case HAL_TIM_ACTIVE_CHANNEL_3:
            channel_impulse = '2';
            channel_index = 1;
            break;

        default:
            return;
    }


    /*
     * Apply the impulse lockout.
     */
    uint32_t now = HAL_GetTick();

    if ((now - last_impulse_time[channel_index])
        < IMPULSE_LOCKOUT_MS)
    {
        return;
    }

    last_impulse_time[channel_index] = now;


    /*
     * Do not generate a timestamp until GNSS/PPS has
     * established UTC.
     */
    if (!unix_time_locked)
    {
        return;
    }


    /*
     * Read TIM2.
     *
     * TIM2:
     *
     *     1 tick = 10 us
     *
     * Therefore:
     *
     *     10 ticks = 100 us
     *
     * and:
     *
     *     TIM2_CNT / 10
     *
     * gives the elapsed time in 100 us units.
     */
    uint32_t timer_ticks =
        __HAL_TIM_GET_COUNTER(&htim2);


    /*
     * Calculate the Unix timestamp in 100 us units.
     */
    uint64_t timestamp_100us =
        current_unix_100us +
        ((uint64_t)timer_ticks /
         TIM2_TICKS_PER_100US);


    /*
     * Convert the uint64_t timestamp manually.
     *
     * This avoids relying on %llu support in the embedded
     * printf implementation.
     */
    char timestamp_string[24];

    uint8_t timestamp_length =
        RTC_Uint64ToString(
            timestamp_100us,
            timestamp_string,
            sizeof(timestamp_string));


    if (timestamp_length == 0)
    {
        return;
    }


    /*
     * Build:
     *
     *     <timestamp>,<channel>\r\n
     *
     * Example:
     *
     *     17868723451234,1
     */
    char buffer[32];

    uint8_t position = 0;


    /*
     * Copy timestamp.
     */
    memcpy(
        &buffer[position],
        timestamp_string,
        timestamp_length);

    position += timestamp_length;


    /*
     * Comma separator.
     */
    buffer[position++] = ',';


    /*
     * Channel number.
     */
    buffer[position++] = channel_impulse;


    /*
     * CR/LF.
     */
    buffer[position++] = '\r';
    buffer[position++] = '\n';


    /*
     * Transmit the timestamp.
     */
    UART_Write(
        COMPUTER_UART,
        (const uint8_t *)buffer,
        position);
}