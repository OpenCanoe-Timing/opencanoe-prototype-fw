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
  uint8_t minutes;       /**< 0-59 */
  uint8_t seconds;        /**< 0-59 */
  uint16_t milliseconds;  /**< 0-999, from the fractional seconds field */
} GNSS_Time_t;

/**
 * @brief UTC calendar date, as reported in an NMEA RMC sentence.
 *
 * NMEA only transmits a two-digit year, so it is interpreted as
 * 2000 + yy.
 */
typedef struct {
  uint8_t day;    /**< 1-31 */
  uint8_t month;   /**< 1-12 */
  uint16_t year;  /**< e.g. 2026 */
} GNSS_Date_t;

/**
 * @brief Last known UTC date/time and fix status.
 */
typedef struct {
  GNSS_Time_t time;
  GNSS_Date_t date;

  /** True if the fix was reported valid ('A') in the most recently
   *  parsed RMC sentence, false if void ('V'). The time/date fields
   *  are still updated either way, since the receiver keeps a clock
   *  running even without a valid fix. */
  bool fix_valid;

} GNSS_DateTime_t;

/**
 * @brief Diagnostic counters, useful for telling a code-side sentence
 *        drop apart from a genuine GNSS signal/receiver issue.
 */
typedef struct {
  /** RMC sentences successfully parsed and applied. */
  uint32_t rmc_parsed;

  /** Sentences (of any type) rejected due to a checksum mismatch,
   *  i.e. bytes were corrupted in transit (noise, framing/baud
   *  error, bad wiring). */
  uint32_t checksum_failures;

  /** Times the line buffer filled up before a terminating '\n' was
   *  seen, so the in-progress line was discarded. Rising in step
   *  with the missed timestamps points at GNSS_LINE_BUFFER_SIZE
   *  being too small, or a missing '\n' somewhere upstream. */
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
 *
 * Registers the module's receive handler against GNSS_UART. Must be
 * called after UART_Init(), since UART_Init() is what starts DMA
 * reception on the port this module listens to.
 */
void GNSS_Init(void);

/**
 * @brief Retrieve the last UTC date/time received from the GNSS receiver.
 *
 * Performs an atomic copy of the internally stored date/time, so this
 * is safe to call from the main loop while new NMEA data is being
 * received in the background.
 *
 * @param datetime Pointer to a GNSS_DateTime_t to populate.
 *
 * @return true  A datetime has been received at least once, and
 *               @p datetime was populated.
 * @return false No RMC sentence has been successfully parsed yet, or
 *               @p datetime was NULL.
 */
bool GNSS_GetLastUTC(GNSS_DateTime_t *datetime);

#endif /* GNSS_H */