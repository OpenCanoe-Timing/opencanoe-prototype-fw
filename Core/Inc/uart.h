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

/**
 * @brief Identifies a UART port available to the application.
 *
 * These identifiers are used by the UART interface instead of exposing
 * the underlying STM32 UART peripherals to the rest of the application.
 */
typedef enum {
  /** UART connection to the host computer. */
  COMPUTER_UART,

  /** UART connection to the GNSS receiver. */
  GNSS_UART,

  /** Number of UART ports available to the application. */
  UART_PORT_COUNT

} UART_Port_t;

/**
 * @brief UART receive callback type.
 *
 * The callback is invoked when new data has been received on a registered
 * UART port. Data may be delivered in chunks rather than one byte at a time.
 *
 * A chunk is bounded either by an idle-line gap on the wire (for example,
 * the pause between NMEA sentences) or by the receive buffer wrapping
 * around, whichever occurs first.
 *
 * The data pointer is only valid for the duration of the callback and
 * must not be retained by the caller.
 *
 * @param uart UART port on which the data was received.
 * @param data Pointer to the received data.
 * @param length Number of bytes in the received data.
 */
typedef void (*UART_RxCallback_t)(UART_Port_t uart, const uint8_t *data,
                                  uint16_t length);

/**
 * @brief Initialise the UART subsystem.
 *
 * Configures and prepares all UART ports used by the application,
 * including their receive and transmit mechanisms.
 *
 * This function must be called before using any other UART functions.
 */
void UART_Init(void);

/**
 * @brief Queue data for transmission over a UART port.
 *
 * The data is copied into the UART transmit mechanism and transmitted
 * asynchronously. The caller may therefore reuse the supplied buffer
 * after this function returns.
 *
 * @param uart UART port to transmit the data on.
 * @param data Pointer to the data to transmit.
 * @param length Number of bytes to transmit.
 *
 * @return true if the data was successfully queued for transmission,
 *         false if the UART transmit queue does not have sufficient space
 *         or the port is otherwise unavailable.
 */
bool UART_Write(UART_Port_t uart, const uint8_t *data, uint16_t length);

/**
 * @brief Register a receive callback for a UART port.
 *
 * The registered callback is invoked whenever a chunk of data is received
 * on the specified UART port.
 *
 * Registering a new callback replaces any callback previously registered
 * for the same UART port.
 *
 * @param uart UART port for which to register the callback.
 * @param callback Function to call when data is received.
 *
 * @return true if the callback was successfully registered,
 *         false if the UART port is invalid or the callback is NULL.
 */
bool UART_RegisterRxCallback(UART_Port_t uart, UART_RxCallback_t callback);

#endif