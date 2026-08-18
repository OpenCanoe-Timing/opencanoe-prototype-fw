/**
 * @file gnss.c
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief GNSS NMEA parsing and PPS-disciplined UTC time handling.
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

#include "gnss.h"
#include "lcd.h"
#include "uart.h"

#include "stm32f4xx_hal.h"

#include <stdlib.h>
#include <string.h>

#define GNSS_LINE_BUFFER_SIZE 128
#define GNSS_FIELD_BUFFER_SIZE 16

#define GNSS_100US_PER_SECOND 10000ULL

/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static char line_buffer[GNSS_LINE_BUFFER_SIZE];
static uint8_t line_pos = 0U;

static volatile GNSS_DateTime_t last_datetime = {0};
static volatile bool has_datetime = false;

static volatile uint64_t last_unix_100us = 0ULL;

static volatile bool pps_locked = false;

static volatile GNSS_Stats_t stats = {0};

/* --------------------------------------------------------------------------
 * Utility functions
 * -------------------------------------------------------------------------- */

static uint8_t GNSS_ParseTwoDigits(const char *digits) {
  return (uint8_t)((digits[0] - '0') * 10 + (digits[1] - '0'));
}

static bool GNSS_GetField(const char *sentence, uint8_t field_index, char *out,
                          size_t out_size) {
  const char *field_start = sentence;
  uint8_t current_field = 0U;

  while (current_field < field_index) {
    field_start = strchr(field_start, ',');

    if (field_start == NULL) {
      return false;
    }

    field_start++;
    current_field++;
  }

  const char *field_end = field_start;

  while (*field_end != '\0' && *field_end != ',' && *field_end != '*') {
    field_end++;
  }

  size_t field_length = (size_t)(field_end - field_start);

  if (field_length >= out_size) {
    return false;
  }

  memcpy(out, field_start, field_length);

  out[field_length] = '\0';

  return true;
}

static bool GNSS_ValidateChecksum(const char *sentence) {
  const char *star = strchr(sentence, '*');

  if (star == NULL || strlen(star) < 3U) {
    return false;
  }

  uint8_t computed = 0U;

  for (const char *c = sentence + 1; c < star; c++) {
    computed ^= (uint8_t)*c;
  }

  char hex[3] = {star[1], star[2], '\0'};

  uint8_t received = (uint8_t)strtoul(hex, NULL, 16);

  return computed == received;
}

/* --------------------------------------------------------------------------
 * Unix date conversion
 * -------------------------------------------------------------------------- */

static int64_t GNSS_DaysFromCivil(int32_t year, uint32_t month, uint32_t day) {
  year -= (month <= 2U);

  const int32_t era = (year >= 0 ? year : year - 399) / 400;

  const uint32_t yoe = (uint32_t)(year - era * 400);

  const uint32_t mp = month + (month > 2U ? (uint32_t)-3 : 9U);

  const uint32_t doy = (153U * mp + 2U) / 5U + day - 1U;

  const uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

  return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

uint64_t GNSS_DateTimeToUnix100us(const GNSS_DateTime_t *datetime) {
  if (datetime == NULL) {
    return 0ULL;
  }

  int64_t days = GNSS_DaysFromCivil(datetime->date.year, datetime->date.month,
                                    datetime->date.day);

  uint64_t seconds = (uint64_t)days * 86400ULL;

  seconds += (uint64_t)datetime->time.hours * 3600ULL;

  seconds += (uint64_t)datetime->time.minutes * 60ULL;

  seconds += (uint64_t)datetime->time.seconds;

  uint64_t timestamp = seconds * GNSS_100US_PER_SECOND;

  timestamp += (uint64_t)(datetime->time.milliseconds * 10U);

  return timestamp;
}

/* --------------------------------------------------------------------------
 * RMC parser
 * -------------------------------------------------------------------------- */

static void GNSS_ParseRMC(const char *sentence) {
  char time_field[GNSS_FIELD_BUFFER_SIZE];
  char status_field[GNSS_FIELD_BUFFER_SIZE];
  char date_field[GNSS_FIELD_BUFFER_SIZE];

  if (!GNSS_GetField(sentence, 1U, time_field, sizeof(time_field))) {
    return;
  }

  if (!GNSS_GetField(sentence, 2U, status_field, sizeof(status_field))) {
    return;
  }

  if (!GNSS_GetField(sentence, 9U, date_field, sizeof(date_field))) {
    return;
  }

  if (strlen(time_field) < 6U || strlen(date_field) != 6U) {
    return;
  }

  GNSS_DateTime_t parsed = {0};

  parsed.time.hours = GNSS_ParseTwoDigits(&time_field[0]);

  parsed.time.minutes = GNSS_ParseTwoDigits(&time_field[2]);

  parsed.time.seconds = GNSS_ParseTwoDigits(&time_field[4]);

  /*
   * NMEA fractional seconds.
   *
   * We retain millisecond resolution in the public
   * GNSS_DateTime_t structure.
   */
  const char *decimal_point = strchr(time_field, '.');

  if (decimal_point != NULL && strlen(decimal_point + 1) >= 2U) {
    parsed.time.milliseconds =
        (uint16_t)(GNSS_ParseTwoDigits(decimal_point + 1) * 10U);
  }

  parsed.date.day = GNSS_ParseTwoDigits(&date_field[0]);

  parsed.date.month = GNSS_ParseTwoDigits(&date_field[2]);

  parsed.date.year = (uint16_t)(2000U + GNSS_ParseTwoDigits(&date_field[4]));

  parsed.fix_valid = (status_field[0] == 'A');

  uint64_t unix_100us = GNSS_DateTimeToUnix100us(&parsed);

  __disable_irq();

  last_datetime = parsed;

  last_unix_100us = unix_100us;

  has_datetime = true;

  stats.rmc_parsed++;

  __enable_irq();
}

/* --------------------------------------------------------------------------
 * Sentence processing
 * -------------------------------------------------------------------------- */

static void GNSS_ProcessSentence(const char *sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 6U) {
    return;
  }

  /*
   * Accept:
   *
   *     GPRMC
   *     GNRMC
   *     GLRMC
   *     GCRMC
   *     etc.
   */
  if (strncmp(&sentence[3], "RMC", 3U) != 0) {
    return;
  }

  if (!GNSS_ValidateChecksum(sentence)) {
    stats.checksum_failures++;
    return;
  }

  GNSS_ParseRMC(sentence);
}

/* --------------------------------------------------------------------------
 * UART receive callback
 * -------------------------------------------------------------------------- */

static void GNSS_RxCallback(UART_Port_t uart, const uint8_t *data,
                            uint16_t length) {
  (void)uart;

  for (uint16_t i = 0U; i < length; i++) {
    uint8_t byte = data[i];

    if (byte == '\r') {
      continue;
    }

    if (byte == '\n') {
      if (line_pos > 0U) {
        line_buffer[line_pos] = '\0';

        GNSS_ProcessSentence(line_buffer);

        line_pos = 0U;
      }

      continue;
    }

    if (line_pos >= (GNSS_LINE_BUFFER_SIZE - 1U)) {
      stats.line_overflows++;

      line_pos = 0U;

      continue;
    }

    line_buffer[line_pos++] = (char)byte;
  }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void GNSS_Init(void) {
  line_pos = 0U;

  pps_locked = false;

  has_datetime = false;

  last_unix_100us = 0ULL;

  UART_RegisterRxCallback(GNSS_UART, GNSS_RxCallback);

  /*
   * Tell the LCD that we do not have PPS lock yet.
   */
  LCD_SetGNSSLock(false);
}

void GNSS_GetStats(GNSS_Stats_t *stats_out) {
  if (stats_out == NULL) {
    return;
  }

  __disable_irq();

  *stats_out = stats;

  __enable_irq();
}

bool GNSS_GetLastUTC(GNSS_DateTime_t *datetime) {
  if (datetime == NULL) {
    return false;
  }

  __disable_irq();

  *datetime = last_datetime;

  bool valid = has_datetime;

  __enable_irq();

  return valid;
}

bool GNSS_GetLastUnix100us(uint64_t *timestamp) {
  if (timestamp == NULL) {
    return false;
  }

  __disable_irq();

  *timestamp = last_unix_100us;

  bool valid = pps_locked;

  __enable_irq();

  return valid;
}

/* --------------------------------------------------------------------------
 * PPS processing
 * -------------------------------------------------------------------------- */

bool GNSS_ProcessPPS(uint32_t pps_tick) {
  (void)pps_tick;

  GNSS_DateTime_t utc;

  /*
   * We cannot establish UTC until an RMC sentence has
   * supplied a date/time.
   */
  if (!GNSS_GetLastUTC(&utc)) {
    return false;
  }

  /*
   * Do not accept an invalid RMC as the UTC source.
   */
  if (!utc.fix_valid) {
    return false;
  }

  /*
   * The RMC timestamp describes the UTC second associated
   * with the receiver's navigation solution.
   *
   * The physical PPS marks the following exact second
   * boundary.
   *
   * Example:
   *
   *     RMC = 12:34:56.xxx
   *
   *     PPS = 12:34:57.000000
   */
  uint64_t unix_100us = GNSS_DateTimeToUnix100us(&utc);

  unix_100us = (unix_100us / GNSS_100US_PER_SECOND) * GNSS_100US_PER_SECOND;

  unix_100us += GNSS_100US_PER_SECOND;

  /*
   * Build the UTC date/time represented by the PPS.
   *
   * Rather than manually handling month/year rollovers,
   * we derive the PPS UTC value from Unix time and retain
   * the Unix timestamp as the authoritative timing value.
   *
   * The displayed calendar date remains the RMC date/time
   * until the next RMC arrives.
   */
  __disable_irq();

  last_unix_100us = unix_100us;

  pps_locked = true;

  __enable_irq();

  /*
   * Tell the LCD that PPS lock has now been achieved and
   * request the UTC display.
   */
  LCD_SetGNSSLock(true);

  LCD_RequestTimeUpdate(&utc);

  return true;
}

bool GNSS_IsPPSLocked(void) {
  bool locked;

  __disable_irq();

  locked = pps_locked;

  __enable_irq();

  return locked;
}