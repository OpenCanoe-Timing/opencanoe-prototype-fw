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

#include <stdint.h>
#include <stdbool.h>

typedef enum
{
    COMPUTER_UART
} UART_Port_t;

bool UART_Write(UART_Port_t uart, const uint8_t *data, uint16_t length);

#endif