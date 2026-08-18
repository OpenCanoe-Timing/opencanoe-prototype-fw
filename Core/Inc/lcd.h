/**
 * @file lcd.h
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief LCD Management.
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

#ifndef LCD_H
#define LCD_H

#include "gnss.h"
#include "stm32f4xx_hal.h"

#include <stdint.h>

/*
 * ============================================================
 * Display dimensions
 * ============================================================
 */

#define LCD_WIDTH 160U
#define LCD_HEIGHT 128U

/*
 * ============================================================
 * RGB565 colours
 * ============================================================
 */

#define LCD_BLACK 0x0000U
#define LCD_WHITE 0xFFFFU
#define LCD_RED 0xF800U
#define LCD_GREEN 0x07E0U
#define LCD_BLUE 0x001FU
#define LCD_CYAN 0x07FFU
#define LCD_YELLOW 0xFFE0U

/*
 * ============================================================
 * Initialisation
 * ============================================================
 */

void LCD_Init(void);

/*
 * ============================================================
 * Basic drawing
 * ============================================================
 */

void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

void LCD_FillScreen(uint16_t color);

void LCD_DrawFastHLine(uint16_t x, uint16_t y, uint16_t width, uint16_t color);

/*
 * ============================================================
 * Text
 * ============================================================
 */

void LCD_SetCursor(uint16_t x, uint16_t y);

void LCD_SetTextColor(uint16_t color);

void LCD_SetTextBackground(uint16_t color);

void LCD_SetTextSize(uint8_t size);

void LCD_WriteChar(char c);

void LCD_Print(const char *str);

/*
 * ============================================================
 * Application display
 * ============================================================
 */

/**
 * @brief Queue a new PPS-disciplined UTC time for display.
 *
 * This function is safe to call from an interrupt.
 *
 * The actual LCD update is performed by LCD_Process().
 */
void LCD_RequestTimeUpdate(const GNSS_DateTime_t *datetime);

/**
 * @brief Queue an impulse indication.
 *
 * The actual LCD update is performed by LCD_Process().
 *
 * The impulse indication remains visible for one second.
 *
 * @param channel Channel character, normally '1' or '2'.
 */
void LCD_RequestImpulse(char channel);

/**
 * @brief Process pending LCD updates.
 *
 * Call this regularly from the main application loop.
 */
void LCD_Process(void);

#endif /* LCD_H */