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

/*
 * ============================================================
 * Configuration
 * ============================================================
 */

/*
 * Longest standard NMEA 0183 sentence is 82 characters
 * including '$' and CR/LF.
 *
 * Give ourselves some additional headroom.
 */
#define GNSS_LINE_BUFFER_SIZE 128U

/*
 * Scratch buffer for individual NMEA fields.
 */
#define GNSS_FIELD_BUFFER_SIZE 16U

/*
 * Number of 100 us intervals in one second.
 */
#define GNSS_100US_PER_SECOND 10000ULL

/*
 * Number of milliseconds in one second.
 */
#define GNSS_MS_PER_SECOND 1000U

/*
 * ============================================================
 * Internal state
 * ============================================================
 */

static char line_buffer[GNSS_LINE_BUFFER_SIZE];
static uint8_t line_pos = 0U;

/*
 * Last RMC received from the GNSS receiver.
 *
 * This is NOT necessarily the current PPS time.
 *
 * It is the GNSS-provided UTC reference from which the PPS
 * determines the actual second boundary.
 */
static volatile GNSS_DateTime_t last_rmc = {0};

static volatile bool has_rmc = false;

/*
 * MCU HAL tick at which the complete RMC sentence was
 * processed.
 *
 * This lets us determine whether the RMC arrived before or
 * after the PPS.
 */
static volatile uint32_t last_rmc_tick = 0U;

/*
 * UTC time established by PPS.
 */
static volatile GNSS_DateTime_t last_datetime = {0};

static volatile uint64_t last_unix_100us = 0ULL;

static volatile bool has_pps_time = false;

/*
 * Diagnostic counters.
 */
static volatile GNSS_Stats_t stats = {0};

/*
 * ============================================================
 * Utility functions
 * ============================================================
 */

/**
 * @brief Parse exactly two ASCII digits into an integer.
 */
static uint8_t GNSS_ParseTwoDigits(const char *digits) {
  return (uint8_t)((digits[0] - '0') * 10 + (digits[1] - '0'));
}

/**
 * @brief Copy the Nth comma-delimited NMEA field.
 */
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

/**
 * @brief Validate an NMEA checksum.
 */
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

/**
 * @brief Calculate days since the Unix epoch.
 *
 * Based on a Gregorian calendar civil-date conversion.
 */
static int64_t GNSS_DaysFromCivil(int32_t year, uint32_t month, uint32_t day) {
  year -= (month <= 2);

  const int32_t era = (year >= 0 ? year : year - 399) / 400;

  const uint32_t yoe = (uint32_t)(year - era * 400);

  const uint32_t mp = month + (month > 2 ? (uint32_t)-3 : 9U);

  const uint32_t doy = (153U * mp + 2U) / 5U + day - 1U;

  const uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;

  return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

/**
 * @brief Convert UTC date/time to Unix time in 100 us units.
 */
uint64_t GNSS_DateTimeToUnix100us(const GNSS_DateTime_t *datetime) {
  if (datetime == NULL) {
    return 0ULL;
  }

  int64_t days = GNSS_DaysFromCivil(datetime->date.year, datetime->date.month,
                                    datetime->date.day);

  int64_t signed_seconds = days * 86400LL;

  signed_seconds += (int64_t)datetime->time.hours * 3600LL;

  signed_seconds += (int64_t)datetime->time.minutes * 60LL;

  signed_seconds += (int64_t)datetime->time.seconds;

  if (signed_seconds < 0) {
    return 0ULL;
  }

  uint64_t timestamp = (uint64_t)signed_seconds * GNSS_100US_PER_SECOND;

  timestamp += (uint64_t)datetime->time.milliseconds * 10ULL;

  return timestamp;
}

/**
 * @brief Add one second to a GNSS date/time.
 */
static void GNSS_AddOneSecond(GNSS_DateTime_t *datetime) {
  datetime->time.milliseconds = 0U;

  datetime->time.seconds++;

  if (datetime->time.seconds < 60U) {
    return;
  }

  datetime->time.seconds = 0U;
  datetime->time.minutes++;

  if (datetime->time.minutes < 60U) {
    return;
  }

  datetime->time.minutes = 0U;
  datetime->time.hours++;

  if (datetime->time.hours < 24U) {
    return;
  }

  datetime->time.hours = 0U;

  /*
   * Advance the calendar date.
   */
  datetime->date.day++;

  /*
   * Determine days in the current month.
   */
  uint8_t days_in_month;

  switch (datetime->date.month) {
  case 2: {
    bool leap = ((datetime->date.year % 4U) == 0U &&
                 (datetime->date.year % 100U) != 0U) ||
                ((datetime->date.year % 400U) == 0U);

    days_in_month = leap ? 29U : 28U;

    break;
  }

  case 4:
  case 6:
  case 9:
  case 11:
    days_in_month = 30U;
    break;

  default:
    days_in_month = 31U;
    break;
  }

  if (datetime->date.day <= days_in_month) {
    return;
  }

  datetime->date.day = 1U;
  datetime->date.month++;

  if (datetime->date.month <= 12U) {
    return;
  }

  datetime->date.month = 1U;
  datetime->date.year++;
}

/*
 * ============================================================
 * RMC parsing
 * ============================================================
 */

/**
 * @brief Parse an RMC sentence.
 */
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

  /*
   * HHMMSS
   */
  parsed.time.hours = GNSS_ParseTwoDigits(&time_field[0]);

  parsed.time.minutes = GNSS_ParseTwoDigits(&time_field[2]);

  parsed.time.seconds = GNSS_ParseTwoDigits(&time_field[4]);

  /*
   * Parse fractional seconds.
   *
   * The public structure only stores milliseconds.
   */
  const char *decimal_point = strchr(time_field, '.');

  if (decimal_point != NULL) {
    const char *fraction = decimal_point + 1;

    size_t fraction_length = strlen(fraction);

    if (fraction_length >= 3U) {
      /*
       * First three digits = milliseconds.
       */
      parsed.time.milliseconds =
          (uint16_t)((fraction[0] - '0') * 100U + (fraction[1] - '0') * 10U +
                     (fraction[2] - '0'));
    } else if (fraction_length == 2U) {
      parsed.time.milliseconds =
          (uint16_t)(GNSS_ParseTwoDigits(fraction) * 10U);
    } else if (fraction_length == 1U) {
      parsed.time.milliseconds = (uint16_t)((fraction[0] - '0') * 100U);
    }
  }

  /*
   * DDMMYY
   */
  parsed.date.day = GNSS_ParseTwoDigits(&date_field[0]);

  parsed.date.month = GNSS_ParseTwoDigits(&date_field[2]);

  parsed.date.year = (uint16_t)(2000U + GNSS_ParseTwoDigits(&date_field[4]));

  parsed.fix_valid = (status_field[0] == 'A');

  /*
   * Record when this RMC was received by the MCU.
   *
   * This is critical for associating the RMC with the
   * following/preceding PPS.
   */
  uint32_t received_tick = HAL_GetTick();

  __disable_irq();

  last_rmc = parsed;
  last_rmc_tick = received_tick;
  has_rmc = true;

  stats.rmc_parsed++;

  __enable_irq();
}

/*
 * ============================================================
 * PPS processing
 * ============================================================
 */

/**
 * @brief Process a GNSS PPS pulse.
 *
 * RMC and PPS relationship:
 *
 *     RMC before PPS:
 *
 *         RMC = 12:34:56.xxx
 *         PPS = 12:34:57.000
 *
 *         Therefore add one second.
 *
 *
 *     RMC after PPS:
 *
 *         PPS = 12:34:56.000
 *         RMC = 12:34:56.xxx
 *
 *         Therefore use the RMC second directly.
 *
 * The comparison is performed using the HAL millisecond tick.
 */
bool GNSS_ProcessPPS(uint32_t pps_tick) {
  GNSS_DateTime_t rmc;
  uint32_t rmc_tick;

  __disable_irq();

  if (!has_rmc) {
    __enable_irq();
    return false;
  }

  rmc = last_rmc;
  rmc_tick = last_rmc_tick;

  __enable_irq();

  /*
   * Determine which UTC second this PPS represents.
   *
   * Unsigned subtraction correctly handles HAL_GetTick()
   * wraparound.
   *
   * If the RMC was received before the PPS, the PPS is
   * considered to be the beginning of the next second.
   */
  GNSS_DateTime_t pps_datetime = rmc;

  if ((int32_t)(pps_tick - rmc_tick) >= 0) {
    /*
     * RMC arrived before PPS.
     */
    GNSS_AddOneSecond(&pps_datetime);
  } else {
    /*
     * RMC arrived after PPS.
     *
     * The PPS corresponds to the second contained in
     * the RMC sentence.
     */
    pps_datetime.time.milliseconds = 0U;
  }

  /*
   * PPS establishes an integer UTC second.
   *
   * Never retain the RMC fractional milliseconds here.
   */
  pps_datetime.time.milliseconds = 0U;

  uint64_t unix_100us = GNSS_DateTimeToUnix100us(&pps_datetime);

  /*
   * Store the PPS-disciplined UTC time.
   */
  __disable_irq();

  last_datetime = pps_datetime;
  last_unix_100us = unix_100us;
  has_pps_time = true;

  __enable_irq();

  /*
   * The PPS is now the authoritative display update event.
   *
   * This means the LCD changes once per second exactly when
   * the PPS establishes the new UTC second.
   */
  LCD_RequestTimeUpdate(&pps_datetime);

  return true;
}

/*
 * ============================================================
 * NMEA processing
 * ============================================================
 */

/**
 * @brief Process one complete NMEA sentence.
 */
static void GNSS_ProcessSentence(const char *sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 6U) {
    return;
  }

  /*
   * Accept:
   *
   *     $GPRMC
   *     $GNRMC
   *     $GLRMC
   *     etc.
   */
  if (strncmp(&sentence[3], "RMC", 3U) != 0) {
    return;
  }

  if (!GNSS_ValidateChecksum(sentence)) {
    __disable_irq();

    stats.checksum_failures++;

    __enable_irq();

    return;
  }

  GNSS_ParseRMC(sentence);
}

/**
 * @brief UART receive callback for the GNSS UART.
 */
static void GNSS_RxCallback(UART_Port_t uart, const uint8_t *data,
                            uint16_t length) {
  (void)uart;

  for (uint16_t i = 0U; i < length; i++) {
    uint8_t byte = data[i];

    /*
     * Ignore carriage return.
     */
    if (byte == '\r') {
      continue;
    }

    /*
     * LF terminates the sentence.
     */
    if (byte == '\n') {
      if (line_pos > 0U) {
        line_buffer[line_pos] = '\0';

        GNSS_ProcessSentence(line_buffer);

        line_pos = 0U;
      }

      continue;
    }

    /*
     * Protect the line buffer.
     */
    if (line_pos >= (GNSS_LINE_BUFFER_SIZE - 1U)) {
      __disable_irq();

      stats.line_overflows++;

      __enable_irq();

      line_pos = 0U;

      continue;
    }

    line_buffer[line_pos++] = (char)byte;
  }
}

/*
 * ============================================================
 * Public API
 * ============================================================
 */

void GNSS_Init(void) {
  line_pos = 0U;

  UART_RegisterRxCallback(GNSS_UART, GNSS_RxCallback);
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

  bool valid = has_pps_time;

  __enable_irq();

  return valid;
}

bool GNSS_GetLastUnix100us(uint64_t *timestamp) {
  if (timestamp == NULL) {
    return false;
  }

  __disable_irq();

  *timestamp = last_unix_100us;

  bool valid = has_pps_time;

  __enable_irq();

  return valid;
}