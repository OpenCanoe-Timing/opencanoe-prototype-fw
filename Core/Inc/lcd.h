/**
 * @file lcd.h
 * @brief LCD management.
 */

#ifndef LCD_H
#define LCD_H

#include "stm32f4xx_hal.h"

#include "gnss.h"

#include <stdbool.h>
#include <stdint.h>

#define LCD_WIDTH 160U
#define LCD_HEIGHT 128U

#define LCD_BLACK 0x0000
#define LCD_WHITE 0xFFFF
#define LCD_RED 0xF800
#define LCD_GREEN 0x07E0
#define LCD_BLUE 0x001F
#define LCD_CYAN 0x07FF
#define LCD_YELLOW 0xFFE0

void LCD_Init(void);

void LCD_Process(void);

void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color);

void LCD_FillScreen(uint16_t color);

void LCD_DrawFastHLine(uint16_t x, uint16_t y, uint16_t width, uint16_t color);

void LCD_SetCursor(uint16_t x, uint16_t y);

void LCD_SetTextColor(uint16_t color);

void LCD_SetTextBackground(uint16_t color);

void LCD_SetTextSize(uint8_t size);

void LCD_WriteChar(char c);

void LCD_Print(const char *str);

/**
 * @brief Request a UTC date/time display update.
 */
void LCD_RequestTimeUpdate(const GNSS_DateTime_t *datetime);

/**
 * @brief Set whether GNSS PPS lock has been achieved.
 *
 * Before the first valid PPS, LCD_Process() displays:
 *
 *     GNSS lock
 *     not achieved!
 */
void LCD_SetGNSSLock(bool locked);

/**
 * @brief Display a timing impulse for one second.
 *
 * @param channel '1' or '2'.
 */
void LCD_RequestImpulse(char channel);

#endif /* LCD_H */