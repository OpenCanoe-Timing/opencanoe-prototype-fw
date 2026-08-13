/**
 * @file uart.h
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief UART Communication header for the application.
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

#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  COMPUTER_UART,
  GNSS_UART,

  UART_PORT_COUNT

} UART_Port_t;

/**
 * @brief UART receive callback type.
 *
 * Called with a chunk of newly received bytes. The chunk is
 * bounded either by an idle-line gap on the wire (e.g. the
 * pause between NMEA sentences) or by the receive buffer
 * wrapping around, whichever comes first. The data pointer
 * is only valid for the duration of the call.
 *
 * @param uart UART port the data was received on.
 * @param data Pointer to the received bytes.
 * @param length Number of bytes received.
 */
typedef void (*UART_RxCallback_t)(UART_Port_t uart, const uint8_t *data,
                                  uint16_t length);

void UART_Init(void);

bool UART_Write(UART_Port_t uart, const uint8_t *data, uint16_t length);

bool UART_RegisterRxCallback(UART_Port_t uart, UART_RxCallback_t callback);

#endif