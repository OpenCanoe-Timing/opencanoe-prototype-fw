/**
 * @file gnss.h
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief GNSS NMEA parsing and last-known UTC time/date storage.
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
 * @brief UTC time of day, as reported in an NMEA RMC sentence.
 */
typedef struct {
  uint8_t hours;        /**< 0-23 */
  uint8_t minutes;      /**< 0-59 */
  uint8_t seconds;      /**< 0-59 */
  uint16_t milliseconds; /**< 0-999 */
} GNSS_Time_t;

/**
 * @brief UTC calendar date, as reported in an NMEA RMC sentence.
 *
 * NMEA only transmits a two-digit year, so it is interpreted as
 * 2000 + yy.
 */
typedef struct {
  uint8_t day;    /**< 1-31 */
  uint8_t month;  /**< 1-12 */
  uint16_t year;  /**< e.g. 2026 */
} GNSS_Date_t;

/**
 * @brief Last known UTC date/time and fix status.
 */
typedef struct {
  GNSS_Time_t time;
  GNSS_Date_t date;

  /**
   * @brief True if the fix was reported valid ('A').
   */
  bool fix_valid;

} GNSS_DateTime_t;

/**
 * @brief Diagnostic counters.
 */
typedef struct {
  /** RMC sentences successfully parsed and applied. */
  uint32_t rmc_parsed;

  /** Sentences rejected due to a checksum mismatch. */
  uint32_t checksum_failures;

  /** Times the line buffer overflowed. */
  uint32_t line_overflows;

} GNSS_Stats_t;

/**
 * @brief Retrieve a snapshot of the module's diagnostic counters.
 *
 * @param stats Pointer to a GNSS_Stats_t to populate.
 */
void GNSS_GetStats(GNSS_Stats_t *stats);

/**
 * @brief Initialise the GNSS module.
 */
void GNSS_Init(void);

/**
 * @brief Retrieve the last UTC date/time received from the GNSS receiver.
 *
 * @param datetime Pointer to a GNSS_DateTime_t to populate.
 *
 * @return true if a valid RMC sentence has been received.
 */
bool GNSS_GetLastUTC(GNSS_DateTime_t *datetime);

/**
 * @brief Convert a UTC date/time to Unix time in 100 us units.
 *
 * Unix epoch is 1970-01-01 00:00:00 UTC.
 *
 * @param datetime UTC date/time to convert.
 *
 * @return Number of 100 us intervals since the Unix epoch.
 */
uint64_t GNSS_DateTimeToUnix100us(const GNSS_DateTime_t *datetime);

/**
 * @brief Get the Unix timestamp represented by the last RMC sentence.
 *
 * The returned value has 100 us resolution.
 *
 * @param timestamp Pointer to receive the timestamp.
 *
 * @return true if a GNSS timestamp is available.
 */
bool GNSS_GetLastUnix100us(uint64_t *timestamp);

#endif /* GNSS_H */