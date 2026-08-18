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
 *
 * The date/time initially comes from an NMEA RMC sentence.
 * The PPS signal is then used to establish the actual UTC
 * second boundary.
 */
typedef struct
{
    uint8_t hours;          /**< 0-23 */
    uint8_t minutes;        /**< 0-59 */
    uint8_t seconds;        /**< 0-59 */
    uint16_t milliseconds;  /**< 0-999 */
} GNSS_Time_t;


/**
 * @brief UTC calendar date.
 *
 * NMEA transmits a two-digit year, which is interpreted as
 * 2000 + yy.
 */
typedef struct
{
    uint8_t day;            /**< 1-31 */
    uint8_t month;          /**< 1-12 */
    uint16_t year;          /**< e.g. 2026 */
} GNSS_Date_t;


/**
 * @brief Last known UTC date/time.
 */
typedef struct
{
    GNSS_Time_t time;
    GNSS_Date_t date;

    /**
     * @brief True when the GNSS receiver reported a valid fix.
     */
    bool fix_valid;

} GNSS_DateTime_t;


/**
 * @brief Diagnostic counters.
 */
typedef struct
{
    /** RMC sentences successfully parsed and applied. */
    uint32_t rmc_parsed;

    /** Sentences rejected due to a checksum mismatch. */
    uint32_t checksum_failures;

    /** Times the line buffer overflowed. */
    uint32_t line_overflows;

} GNSS_Stats_t;


/**
 * @brief Initialise the GNSS module.
 */
void GNSS_Init(void);


/**
 * @brief Retrieve a snapshot of the GNSS diagnostic counters.
 *
 * @param stats Pointer to a GNSS_Stats_t to populate.
 */
void GNSS_GetStats(GNSS_Stats_t *stats);


/**
 * @brief Retrieve the last UTC date/time established by PPS.
 *
 * This is the time currently considered to be the UTC time
 * at the beginning of the most recent PPS-defined second.
 *
 * @param datetime Pointer to a GNSS_DateTime_t to populate.
 *
 * @return true if UTC has been established by a PPS.
 */
bool GNSS_GetLastUTC(GNSS_DateTime_t *datetime);


/**
 * @brief Convert a UTC date/time to Unix time in 100 us units.
 *
 * Unix epoch is:
 *
 *     1970-01-01 00:00:00 UTC
 *
 * @param datetime UTC date/time to convert.
 *
 * @return Number of 100 us intervals since the Unix epoch.
 */
uint64_t GNSS_DateTimeToUnix100us(
    const GNSS_DateTime_t *datetime);


/**
 * @brief Retrieve the current PPS-disciplined Unix timestamp.
 *
 * The returned timestamp represents the beginning of the
 * current UTC second established by PPS.
 *
 * @param timestamp Pointer to receive the timestamp.
 *
 * @return true if UTC has been established by PPS.
 */
bool GNSS_GetLastUnix100us(uint64_t *timestamp);


/**
 * @brief Apply a GNSS PPS pulse to the UTC clock.
 *
 * This function must be called from the PPS interrupt.
 *
 * The most recently received RMC sentence is compared with
 * the PPS reception time to determine which UTC second the
 * pulse represents.
 *
 * Once the PPS has established UTC, the LCD is requested to
 * display the new time.
 *
 * @param pps_tick HAL tick corresponding to the PPS event.
 *
 * @return true if the PPS successfully established UTC.
 */
bool GNSS_ProcessPPS(uint32_t pps_tick);

#endif /* GNSS_H */