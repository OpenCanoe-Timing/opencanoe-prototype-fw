/**
 * @file gnss.c
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief GNSS NMEA parsing and UTC timestamp handling.
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
#include "uart.h"
#include "stm32f4xx_hal.h"

#include <stdlib.h>
#include <string.h>

/* Longest standard NMEA 0183 sentence is 82 characters including the
 * leading '$' and trailing <CR><LF>. Give ourselves some headroom. */
#define GNSS_LINE_BUFFER_SIZE 128

/* Scratch buffer for individual NMEA fields. */
#define GNSS_FIELD_BUFFER_SIZE 16

/* Number of 100 us intervals in one second. */
#define GNSS_100US_PER_SECOND 10000ULL

static char line_buffer[GNSS_LINE_BUFFER_SIZE];
static uint8_t line_pos = 0;

static volatile GNSS_DateTime_t last_datetime = {0};
static volatile bool has_datetime = false;

static volatile uint64_t last_unix_100us = 0;
static volatile GNSS_Stats_t stats = {0};


/**
 * @brief Parse exactly two ASCII digits into an integer.
 */
static uint8_t GNSS_ParseTwoDigits(const char *digits)
{
  return (uint8_t)((digits[0] - '0') * 10 +
                   (digits[1] - '0'));
}


/**
 * @brief Copy the Nth comma-delimited field from an NMEA sentence.
 */
static bool GNSS_GetField(const char *sentence,
                          uint8_t field_index,
                          char *out,
                          size_t out_size)
{
  const char *field_start = sentence;
  uint8_t current_field = 0;

  while (current_field < field_index)
  {
    field_start = strchr(field_start, ',');

    if (field_start == NULL)
    {
      return false;
    }

    field_start++;
    current_field++;
  }

  const char *field_end = field_start;

  while (*field_end != '\0' &&
         *field_end != ',' &&
         *field_end != '*')
  {
    field_end++;
  }

  size_t field_length =
      (size_t)(field_end - field_start);

  if (field_length >= out_size)
  {
    return false;
  }

  memcpy(out, field_start, field_length);
  out[field_length] = '\0';

  return true;
}


/**
 * @brief Validate an NMEA checksum.
 */
static bool GNSS_ValidateChecksum(const char *sentence)
{
  const char *star = strchr(sentence, '*');

  if (star == NULL || strlen(star) < 3)
  {
    return false;
  }

  uint8_t computed = 0;

  for (const char *c = sentence + 1; c < star; c++)
  {
    computed ^= (uint8_t)*c;
  }

  char hex[3] = {
    star[1],
    star[2],
    '\0'
  };

  uint8_t received =
      (uint8_t)strtoul(hex, NULL, 16);

  return computed == received;
}


/**
 * @brief Calculate the number of days since the Unix epoch.
 *
 * Based on a Gregorian calendar civil-date conversion.
 */
static int64_t GNSS_DaysFromCivil(int32_t year,
                                  uint32_t month,
                                  uint32_t day)
{
  year -= (month <= 2);

  const int32_t era =
      (year >= 0 ? year : year - 399) / 400;

  const uint32_t yoe =
      (uint32_t)(year - era * 400);

  const uint32_t mp =
      month + (month > 2 ? (uint32_t)-3 : 9);

  const uint32_t doy =
      (153 * mp + 2) / 5 + day - 1;

  const uint32_t doe =
      yoe * 365 +
      yoe / 4 -
      yoe / 100 +
      doy;

  return (int64_t)era * 146097 +
         (int64_t)doe -
         719468;
}


/**
 * @brief Convert a GNSS date/time to Unix time in 100 us units.
 */
uint64_t GNSS_DateTimeToUnix100us(
    const GNSS_DateTime_t *datetime)
{
  if (datetime == NULL)
  {
    return 0;
  }

  int64_t days = GNSS_DaysFromCivil(
      datetime->date.year,
      datetime->date.month,
      datetime->date.day);

  uint64_t seconds =
      (uint64_t)days * 86400ULL;

  seconds +=
      (uint64_t)datetime->time.hours * 3600ULL;

  seconds +=
      (uint64_t)datetime->time.minutes * 60ULL;

  seconds +=
      (uint64_t)datetime->time.seconds;

  /*
   * Convert seconds to 100 us units.
   *
   * 1 second = 10,000 × 100 us.
   */
  uint64_t timestamp =
      seconds * GNSS_100US_PER_SECOND;

  /*
   * Convert milliseconds to 100 us.
   *
   * 1 ms = 10 × 100 us.
   */
  timestamp +=
      (uint64_t)(datetime->time.milliseconds * 10U);

  return timestamp;
}


/**
 * @brief Parse an RMC sentence's UTC time and date.
 */
static void GNSS_ParseRMC(const char *sentence)
{
  char time_field[GNSS_FIELD_BUFFER_SIZE];
  char status_field[GNSS_FIELD_BUFFER_SIZE];
  char date_field[GNSS_FIELD_BUFFER_SIZE];

  if (!GNSS_GetField(sentence,
                     1,
                     time_field,
                     sizeof(time_field)))
  {
    return;
  }

  if (!GNSS_GetField(sentence,
                     2,
                     status_field,
                     sizeof(status_field)))
  {
    return;
  }

  if (!GNSS_GetField(sentence,
                     9,
                     date_field,
                     sizeof(date_field)))
  {
    return;
  }

  if (strlen(time_field) < 6 ||
      strlen(date_field) != 6)
  {
    return;
  }

  GNSS_DateTime_t parsed = {0};

  parsed.time.hours =
      GNSS_ParseTwoDigits(&time_field[0]);

  parsed.time.minutes =
      GNSS_ParseTwoDigits(&time_field[2]);

  parsed.time.seconds =
      GNSS_ParseTwoDigits(&time_field[4]);

  /*
   * Parse fractional seconds.
   *
   * For example:
   *
   * 123456.78
   *
   * becomes:
   *
   * 123456 ms + 780 us
   *
   * The public GNSS_DateTime_t stores milliseconds, so the
   * fractional part is converted to milliseconds.
   */
  const char *decimal_point =
      strchr(time_field, '.');

  if (decimal_point != NULL &&
      strlen(decimal_point + 1) >= 2)
  {
    parsed.time.milliseconds =
        (uint16_t)(
            GNSS_ParseTwoDigits(decimal_point + 1) * 10U
        );
  }

  parsed.date.day =
      GNSS_ParseTwoDigits(&date_field[0]);

  parsed.date.month =
      GNSS_ParseTwoDigits(&date_field[2]);

  parsed.date.year =
      (uint16_t)(
          2000 +
          GNSS_ParseTwoDigits(&date_field[4])
      );

  parsed.fix_valid =
      (status_field[0] == 'A');

  uint64_t unix_100us =
      GNSS_DateTimeToUnix100us(&parsed);

  __disable_irq();

  last_datetime = parsed;
  last_unix_100us = unix_100us;
  has_datetime = true;

  stats.rmc_parsed++;

  __enable_irq();
}


/**
 * @brief Process one complete NMEA sentence.
 */
static void GNSS_ProcessSentence(const char *sentence)
{
  if (sentence[0] != '$' ||
      strlen(sentence) < 6)
  {
    return;
  }

  /*
   * "$GPRMC", "$GNRMC", "$GLRMC", etc.
   */
  if (strncmp(&sentence[3], "RMC", 3) != 0)
  {
    return;
  }

  if (!GNSS_ValidateChecksum(sentence))
  {
    stats.checksum_failures++;
    return;
  }

  GNSS_ParseRMC(sentence);
}


/**
 * @brief UART receive callback registered for GNSS_UART.
 */
static void GNSS_RxCallback(UART_Port_t uart,
                            const uint8_t *data,
                            uint16_t length)
{
  (void)uart;

  for (uint16_t i = 0; i < length; i++)
  {
    uint8_t byte = data[i];

    if (byte == '\r')
    {
      continue;
    }

    if (byte == '\n')
    {
      if (line_pos > 0)
      {
        line_buffer[line_pos] = '\0';

        GNSS_ProcessSentence(line_buffer);

        line_pos = 0;
      }

      continue;
    }

    if (line_pos >=
        (GNSS_LINE_BUFFER_SIZE - 1))
    {
      stats.line_overflows++;
      line_pos = 0;
      continue;
    }

    line_buffer[line_pos++] =
        (char)byte;
  }
}


void GNSS_Init(void)
{
  line_pos = 0;

  UART_RegisterRxCallback(
      GNSS_UART,
      GNSS_RxCallback);
}


void GNSS_GetStats(GNSS_Stats_t *stats_out)
{
  if (stats_out == NULL)
  {
    return;
  }

  __disable_irq();

  *stats_out = stats;

  __enable_irq();
}


bool GNSS_GetLastUTC(GNSS_DateTime_t *datetime)
{
  if (datetime == NULL)
  {
    return false;
  }

  __disable_irq();

  *datetime = last_datetime;

  bool valid = has_datetime;

  __enable_irq();

  return valid;
}


bool GNSS_GetLastUnix100us(uint64_t *timestamp)
{
  if (timestamp == NULL)
  {
    return false;
  }

  __disable_irq();

  *timestamp = last_unix_100us;

  bool valid = has_datetime;

  __enable_irq();

  return valid;
}