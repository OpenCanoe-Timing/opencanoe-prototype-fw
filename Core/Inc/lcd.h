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
 */

#ifndef LCD_H
#define LCD_H

#include "main.h"
#include "gnss.h"

#include <stdint.h>


/* --------------------------------------------------------------------------
 * Display size
 * -------------------------------------------------------------------------- */

#define LCD_WIDTH   160
#define LCD_HEIGHT  128


/* --------------------------------------------------------------------------
 * ST7735 colours - RGB565
 * -------------------------------------------------------------------------- */

#define LCD_BLACK       0x0000
#define LCD_WHITE       0xFFFF
#define LCD_RED         0xF800
#define LCD_GREEN       0x07E0
#define LCD_BLUE        0x001F
#define LCD_CYAN        0x07FF
#define LCD_MAGENTA     0xF81F
#define LCD_YELLOW      0xFFE0


/* --------------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise the ST7735 LCD.
 */
void LCD_Init(void);


/* --------------------------------------------------------------------------
 * Basic drawing
 * -------------------------------------------------------------------------- */

/**
 * @brief Draw a single pixel.
 *
 * This function is intended for individual pixel operations.
 * For drawing larger objects, use the optimized drawing functions
 * where possible.
 */
void LCD_DrawPixel(
    uint16_t x,
    uint16_t y,
    uint16_t color
);


/**
 * @brief Fill the entire display with one colour.
 *
 * This is considerably faster than drawing individual pixels because
 * the entire display is transferred using a continuous SPI transaction.
 */
void LCD_FillScreen(
    uint16_t color
);


/**
 * @brief Draw a horizontal line.
 *
 * The line is transferred as one continuous SPI operation.
 */
void LCD_DrawFastHLine(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t color
);


/* --------------------------------------------------------------------------
 * Text
 * -------------------------------------------------------------------------- */

/**
 * @brief Set the text cursor position.
 *
 * The cursor specifies the top-left corner of the next character.
 */
void LCD_SetCursor(
    uint16_t x,
    uint16_t y
);


/**
 * @brief Set the colour used for text.
 */
void LCD_SetTextColor(
    uint16_t color
);


/**
 * @brief Set the text scaling factor.
 *
 * A size of 1 produces the native 5x7 font.
 * Larger values scale each font pixel accordingly.
 */
void LCD_SetTextSize(
    uint8_t size
);


/**
 * @brief Draw one ASCII character.
 *
 * The character is rendered using a single address window and
 * continuous SPI transfer rather than issuing a separate LCD
 * transaction for every individual pixel.
 *
 * The character cell is six font pixels wide. Blank pixels inside
 * the character are written using LCD_BLACK so that an updated
 * character does not leave remnants of the previous character.
 */
void LCD_WriteChar(
    char c
);


/**
 * @brief Draw a null-terminated string.
 *
 * Newline characters move the cursor to the beginning of the
 * next text row.
 */
void LCD_Print(
    const char *str
);


/* --------------------------------------------------------------------------
 * GNSS time display
 * -------------------------------------------------------------------------- */

/**
 * @brief Request that the LCD display the latest GNSS UTC date/time.
 *
 * The date/time is copied into the LCD module and the actual display
 * operation is deferred until LCD_Process() is called.
 *
 * Calling this function does not perform any SPI transfers and is
 * therefore suitable for use from the GNSS receive path.
 */
void LCD_RequestTimeUpdate(
    const GNSS_DateTime_t *datetime
);


/**
 * @brief Process a pending LCD date/time update.
 *
 * Only characters that have changed since the previous update are
 * rewritten. This significantly reduces the amount of SPI traffic
 * when the GNSS time is updated once per second.
 *
 * This function performs blocking SPI transfers and should therefore
 * be called from the main application context rather than an ISR.
 */
void LCD_Process(void);


#endif /* LCD_H */