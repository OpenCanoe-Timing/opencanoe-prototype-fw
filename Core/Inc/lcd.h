#ifndef LCD_H
#define LCD_H

#include "main.h"
#include "gnss.h"
#include <stdint.h>

/* Display size */
#define LCD_WIDTH   160
#define LCD_HEIGHT  128

/* ST7735 colors - RGB565 */
#define LCD_BLACK       0x0000
#define LCD_WHITE       0xFFFF
#define LCD_RED         0xF800
#define LCD_GREEN       0x07E0
#define LCD_BLUE        0x001F
#define LCD_CYAN        0x07FF
#define LCD_MAGENTA     0xF81F
#define LCD_YELLOW      0xFFE0

/* Initialise display */
void LCD_Init(void);

/* Basic drawing */
void LCD_FillScreen(uint16_t color);
void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawFastHLine(uint16_t x, uint16_t y,
                       uint16_t width, uint16_t color);

/* Text */
void LCD_SetCursor(uint16_t x, uint16_t y);
void LCD_SetTextColor(uint16_t color);
void LCD_SetTextSize(uint8_t size);
void LCD_WriteChar(char c);
void LCD_Print(const char *str);

/* Time display */
void LCD_RequestTimeUpdate(const GNSS_DateTime_t *datetime);

void LCD_Process(void);

#endif