/**
 * @file gnss.h
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief GNSS NMEA parsing and PPS-disciplined UTC time storage.
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

#ifndef GNSS_H
#define GNSS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief UTC time of day.
 */
typedef struct {
  uint8_t hours;
  uint8_t minutes;
  uint8_t seconds;
  uint16_t milliseconds;

} GNSS_Time_t;

/**
 * @brief UTC calendar date.
 */
typedef struct {
  uint8_t day;
  uint8_t month;
  uint16_t year;

} GNSS_Date_t;

/**
 * @brief UTC date/time and fix status.
 */
typedef struct {
  GNSS_Time_t time;
  GNSS_Date_t date;

  bool fix_valid;

} GNSS_DateTime_t;

/**
 * @brief Diagnostic counters.
 */
typedef struct {
  uint32_t rmc_parsed;
  uint32_t checksum_failures;
  uint32_t line_overflows;

} GNSS_Stats_t;

/**
 * @brief Initialise the GNSS module.
 */
void GNSS_Init(void);

/**
 * @brief Retrieve GNSS statistics.
 */
void GNSS_GetStats(GNSS_Stats_t *stats);

/**
 * @brief Retrieve the last RMC UTC date/time.
 */
bool GNSS_GetLastUTC(GNSS_DateTime_t *datetime);

/**
 * @brief Convert UTC date/time to Unix time in 100 us units.
 */
uint64_t GNSS_DateTimeToUnix100us(const GNSS_DateTime_t *datetime);

/**
 * @brief Retrieve the current PPS-disciplined Unix timestamp.
 *
 * This is the timestamp corresponding to the beginning of
 * the current UTC second.
 */
bool GNSS_GetLastUnix100us(uint64_t *timestamp);

/**
 * @brief Process a physical GNSS PPS event.
 *
 * The latest valid RMC sentence is associated with the
 * physical PPS and the UTC timestamp is advanced to the
 * corresponding second boundary.
 *
 * @param pps_tick HAL millisecond tick at the PPS interrupt.
 *
 * @return true if the PPS established a valid UTC lock.
 */
bool GNSS_ProcessPPS(uint32_t pps_tick);

/**
 * @brief Determine whether GNSS PPS UTC lock has been achieved.
 */
bool GNSS_IsPPSLocked(void);

#endif /* GNSS_H */