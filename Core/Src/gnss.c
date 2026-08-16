/**
 * @file gnss.c
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

#include "gnss.h"
#include "uart.h"
#include "stm32f4xx_hal.h"

#include <stdlib.h>
#include <string.h>

/* Longest standard NMEA 0183 sentence is 82 characters including the
 * leading '$' and trailing <CR><LF>. Give ourselves a little headroom. */
#define GNSS_LINE_BUFFER_SIZE 128

/* Small scratch buffer used when copying out a single comma-delimited
 * field. Longest field we care about (hhmmss.ss) is 9 characters. */
#define GNSS_FIELD_BUFFER_SIZE 16

static char line_buffer[GNSS_LINE_BUFFER_SIZE];
static uint8_t line_pos = 0;

static volatile GNSS_DateTime_t last_datetime = {0};
static volatile bool has_datetime = false;

static volatile GNSS_Stats_t stats = {0};

/**
 * @brief Parse exactly two ASCII digits into an integer.
 *
 * @param digits Pointer to two ASCII digit characters.
 *
 * @return Value 0-99. Behaviour is undefined if @p digits does not
 *         point to two digit characters.
 */
static uint8_t GNSS_ParseTwoDigits(const char *digits) {
  return (uint8_t)((digits[0] - '0') * 10 + (digits[1] - '0'));
}

/**
 * @brief Copy the Nth comma-delimited field out of an NMEA sentence.
 *
 * Field 0 is the sentence identifier itself (e.g. "$GPRMC"). Fields
 * are copied without the surrounding commas; an empty field (two
 * commas back-to-back) yields an empty string.
 *
 * @param sentence Null-terminated NMEA sentence.
 * @param field_index Zero-based field index to extract.
 * @param out Buffer to receive the field, always null-terminated.
 * @param out_size Size of @p out in bytes.
 *
 * @return true  Field was found and copied (possibly empty).
 * @return false Field index is beyond the end of the sentence, or the
 *               field did not fit in @p out.
 */
static bool GNSS_GetField(const char *sentence, uint8_t field_index,
                          char *out, size_t out_size) {
  const char *field_start = sentence;
  uint8_t current_field = 0;

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
 * @brief Validate the trailing checksum of an NMEA sentence.
 *
 * The checksum is the XOR of every byte between (but not including)
 * the leading '$' and the trailing '*', expressed as two upper-case
 * hex digits after the '*'.
 *
 * @param sentence Null-terminated NMEA sentence, starting with '$'.
 *
 * @return true  Checksum present and matches.
 * @return false Checksum missing, malformed, or mismatched.
 */
static bool GNSS_ValidateChecksum(const char *sentence) {
  const char *star = strchr(sentence, '*');

  if (star == NULL || strlen(star) < 3) {
    return false;
  }

  uint8_t computed = 0;

  for (const char *c = sentence + 1; c < star; c++) {
    computed ^= (uint8_t)*c;
  }

  char hex[3] = {star[1], star[2], '\0'};
  uint8_t received = (uint8_t)strtoul(hex, NULL, 16);

  return computed == received;
}

/**
 * @brief Parse an RMC sentence's UTC time and date fields.
 *
 * On success, atomically updates the module's stored last-known
 * date/time. Sentences with a time field too short to contain at
 * least hhmmss, or a date field not exactly 6 characters, are
 * rejected without updating anything.
 *
 * @param sentence Null-terminated, checksum-validated RMC sentence.
 */
static void GNSS_ParseRMC(const char *sentence) {
  char time_field[GNSS_FIELD_BUFFER_SIZE];
  char status_field[GNSS_FIELD_BUFFER_SIZE];
  char date_field[GNSS_FIELD_BUFFER_SIZE];

  if (!GNSS_GetField(sentence, 1, time_field, sizeof(time_field))) {
    return;
  }

  if (!GNSS_GetField(sentence, 2, status_field, sizeof(status_field))) {
    return;
  }

  if (!GNSS_GetField(sentence, 9, date_field, sizeof(date_field))) {
    return;
  }

  if (strlen(time_field) < 6 || strlen(date_field) != 6) {
    return;
  }

  GNSS_DateTime_t parsed = {0};

  parsed.time.hours = GNSS_ParseTwoDigits(&time_field[0]);
  parsed.time.minutes = GNSS_ParseTwoDigits(&time_field[2]);
  parsed.time.seconds = GNSS_ParseTwoDigits(&time_field[4]);

  /* Fractional seconds, e.g. the ".00" in "003053.00", given as
   * hundredths of a second. */
  const char *decimal_point = strchr(time_field, '.');

  if (decimal_point != NULL && strlen(decimal_point + 1) >= 2) {
    parsed.time.milliseconds =
        (uint16_t)(GNSS_ParseTwoDigits(decimal_point + 1) * 10);
  }

  parsed.date.day = GNSS_ParseTwoDigits(&date_field[0]);
  parsed.date.month = GNSS_ParseTwoDigits(&date_field[2]);
  parsed.date.year = (uint16_t)(2000 + GNSS_ParseTwoDigits(&date_field[4]));

  parsed.fix_valid = (status_field[0] == 'A');

  __disable_irq();
  last_datetime = parsed;
  has_datetime = true;
  stats.rmc_parsed++;
  __enable_irq();
}

/**
 * @brief Process a single, complete, null-terminated NMEA sentence.
 *
 * Filters for RMC sentences regardless of talker ID (GP, GN, GL, GA,
 * etc. all report RMC at the same field position), validates the
 * checksum, then hands off to GNSS_ParseRMC().
 *
 * @param sentence Null-terminated line, without the trailing CR/LF.
 */
static void GNSS_ProcessSentence(const char *sentence) {
  if (sentence[0] != '$' || strlen(sentence) < 6) {
    return;
  }

  /* Sentence type sits at offset 3, after the 2-character talker ID
   * (e.g. "$GPRMC" -> "RMC" starts at index 3). */
  if (strncmp(&sentence[3], "RMC", 3) != 0) {
    return;
  }

  if (!GNSS_ValidateChecksum(sentence)) {
    stats.checksum_failures++;
    return;
  }

  GNSS_ParseRMC(sentence);
}

/**
 * @brief UART receive callback registered for GNSS_UART.
 *
 * Accumulates incoming bytes into a line buffer and processes each
 * complete sentence as it is terminated by '\n'. '\r' is dropped.
 * If a sentence exceeds the line buffer (e.g. due to a dropped byte
 * corrupting framing), the buffer is discarded and reception resyncs
 * on the next line.
 *
 * @param uart Port the data arrived on (always GNSS_UART, since this
 *             callback is only ever registered against that port).
 * @param data Pointer to the newly received bytes.
 * @param length Number of bytes in @p data.
 */
static void GNSS_RxCallback(UART_Port_t uart, const uint8_t *data,
                            uint16_t length) {
  (void)uart;

  for (uint16_t i = 0; i < length; i++) {
    uint8_t byte = data[i];

    if (byte == '\r') {
      continue;
    }

    if (byte == '\n') {
      if (line_pos > 0) {
        line_buffer[line_pos] = '\0';
        GNSS_ProcessSentence(line_buffer);
        line_pos = 0;
      }

      continue;
    }

    if (line_pos >= (GNSS_LINE_BUFFER_SIZE - 1)) {
      /* Line too long / framing lost: drop it and resync on the
       * next newline. */
      stats.line_overflows++;
      line_pos = 0;
      continue;
    }

    line_buffer[line_pos++] = (char)byte;
  }
}

void GNSS_Init(void) {
  line_pos = 0;

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
  bool valid = has_datetime;
  __enable_irq();

  return valid;
}