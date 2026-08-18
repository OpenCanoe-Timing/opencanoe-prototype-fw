/**
 * @file lcd.c
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

#include "lcd.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern SPI_HandleTypeDef hspi2;

/*
 * ============================================================
 * ST7735 pins
 * ============================================================
 */

#define LCD_CS_GPIO_Port GPIOC
#define LCD_CS_Pin GPIO_PIN_9

#define LCD_DC_GPIO_Port GPIOC
#define LCD_DC_Pin GPIO_PIN_8

#define LCD_RST_GPIO_Port GPIOC
#define LCD_RST_Pin GPIO_PIN_7

/*
 * ============================================================
 * ST7735 commands
 * ============================================================
 */

#define ST7735_SWRESET 0x01U
#define ST7735_SLPOUT 0x11U
#define ST7735_COLMOD 0x3AU
#define ST7735_MADCTL 0x36U
#define ST7735_CASET 0x2AU
#define ST7735_RASET 0x2BU
#define ST7735_RAMWR 0x2CU
#define ST7735_DISPON 0x29U
#define ST7735_INVOFF 0x20U

/*
 * ============================================================
 * Application timing
 * ============================================================
 */

#define LCD_IMPULSE_DISPLAY_MS 1000U

/*
 * ============================================================
 * Internal state
 * ============================================================
 */

static uint16_t cursor_x = 0U;
static uint16_t cursor_y = 0U;

static uint16_t text_color = LCD_WHITE;
static uint16_t text_background = LCD_BLACK;

static uint8_t text_size = 1U;

/*
 * Last PPS-disciplined UTC time.
 */
static volatile GNSS_DateTime_t display_datetime = {0};

static volatile bool time_update_pending = false;

/*
 * Impulse display state.
 */
static volatile bool impulse_pending = false;

static volatile char impulse_channel = '\0';

static volatile uint32_t impulse_start_time = 0U;

/*
 * Which screen is currently displayed.
 */
typedef enum {
  LCD_SCREEN_TIME = 0,
  LCD_SCREEN_IMPULSE

} LCD_Screen_t;

static LCD_Screen_t current_screen = LCD_SCREEN_TIME;

/*
 * ============================================================
 * GPIO
 * ============================================================
 */

static void LCD_CS_Low(void) {
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
}

static void LCD_CS_High(void) {
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

static void LCD_DC_Low(void) {
  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
}

static void LCD_DC_High(void) {
  HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
}

static void LCD_Reset(void) {
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);

  HAL_Delay(20U);

  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);

  HAL_Delay(120U);
}

/*
 * ============================================================
 * SPI
 * ============================================================
 */

static void LCD_WriteCommand(uint8_t command) {
  LCD_CS_Low();
  LCD_DC_Low();

  HAL_SPI_Transmit(&hspi2, &command, 1U, HAL_MAX_DELAY);

  LCD_CS_High();
}

static void LCD_WriteData(uint8_t *data, uint16_t length) {
  LCD_CS_Low();
  LCD_DC_High();

  HAL_SPI_Transmit(&hspi2, data, length, HAL_MAX_DELAY);

  LCD_CS_High();
}

static void LCD_WriteDataByte(uint8_t data) { LCD_WriteData(&data, 1U); }

/*
 * ============================================================
 * ST7735 address window
 * ============================================================
 */

static void LCD_SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1,
                                 uint16_t y1) {
  uint8_t data[4];

  /*
   * Column address.
   */
  LCD_WriteCommand(ST7735_CASET);

  data[0] = (uint8_t)(x0 >> 8);
  data[1] = (uint8_t)(x0 & 0xFFU);
  data[2] = (uint8_t)(x1 >> 8);
  data[3] = (uint8_t)(x1 & 0xFFU);

  LCD_WriteData(data, 4U);

  /*
   * Row address.
   */
  LCD_WriteCommand(ST7735_RASET);

  data[0] = (uint8_t)(y0 >> 8);
  data[1] = (uint8_t)(y0 & 0xFFU);
  data[2] = (uint8_t)(y1 >> 8);
  data[3] = (uint8_t)(y1 & 0xFFU);

  LCD_WriteData(data, 4U);

  /*
   * Start RAM write.
   */
  LCD_WriteCommand(ST7735_RAMWR);
}

/*
 * ============================================================
 * Initialisation
 * ============================================================
 */

void LCD_Init(void) {
  LCD_Reset();

  /*
   * Software reset.
   */
  LCD_WriteCommand(ST7735_SWRESET);

  HAL_Delay(150U);

  /*
   * Exit sleep.
   */
  LCD_WriteCommand(ST7735_SLPOUT);

  HAL_Delay(120U);

  /*
   * RGB565.
   */
  LCD_WriteCommand(ST7735_COLMOD);

  LCD_WriteDataByte(0x05U);

  HAL_Delay(10U);

  /*
   * Display orientation.
   */
  LCD_WriteCommand(ST7735_MADCTL);

  LCD_WriteDataByte(0xA0U);

  /*
   * Disable inversion.
   */
  LCD_WriteCommand(ST7735_INVOFF);

  /*
   * Display on.
   */
  LCD_WriteCommand(ST7735_DISPON);

  HAL_Delay(100U);

  /*
   * Default text state.
   */
  cursor_x = 0U;
  cursor_y = 0U;

  text_color = LCD_WHITE;
  text_background = LCD_BLACK;

  text_size = 1U;

  /*
   * Start with a clean screen.
   */
  LCD_FillScreen(LCD_BLACK);
}

/*
 * ============================================================
 * Basic drawing
 * ============================================================
 */

void LCD_DrawPixel(uint16_t x, uint16_t y, uint16_t color) {
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) {
    return;
  }

  LCD_SetAddressWindow(x, y, x, y);

  uint8_t data[2];

  data[0] = (uint8_t)(color >> 8);

  data[1] = (uint8_t)(color & 0xFFU);

  LCD_WriteData(data, 2U);
}

void LCD_FillScreen(uint16_t color) {
  LCD_SetAddressWindow(0U, 0U, LCD_WIDTH - 1U, LCD_HEIGHT - 1U);

  uint8_t buffer[128];

  uint8_t high = (uint8_t)(color >> 8);

  uint8_t low = (uint8_t)(color & 0xFFU);

  for (uint16_t i = 0U; i < sizeof(buffer); i += 2U) {
    buffer[i] = high;
    buffer[i + 1U] = low;
  }

  LCD_CS_Low();
  LCD_DC_High();

  uint32_t pixel_count = (uint32_t)LCD_WIDTH * LCD_HEIGHT;

  uint32_t pixels_sent = 0U;

  while (pixels_sent < pixel_count) {
    uint32_t remaining = pixel_count - pixels_sent;

    uint16_t pixels = (remaining >= 64U) ? 64U : (uint16_t)remaining;

    HAL_SPI_Transmit(&hspi2, buffer, (uint16_t)(pixels * 2U), HAL_MAX_DELAY);

    pixels_sent += pixels;
  }

  LCD_CS_High();
}

void LCD_DrawFastHLine(uint16_t x, uint16_t y, uint16_t width, uint16_t color) {
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) {
    return;
  }

  if (width > LCD_WIDTH - x) {
    width = LCD_WIDTH - x;
  }

  if (width == 0U) {
    return;
  }

  LCD_SetAddressWindow(x, y, x + width - 1U, y);

  uint8_t buffer[128];

  uint8_t high = (uint8_t)(color >> 8);

  uint8_t low = (uint8_t)(color & 0xFFU);

  for (uint16_t i = 0U; i < sizeof(buffer); i += 2U) {
    buffer[i] = high;
    buffer[i + 1U] = low;
  }

  LCD_CS_Low();
  LCD_DC_High();

  uint16_t remaining = width;

  while (remaining > 0U) {
    uint16_t pixels = (remaining > 64U) ? 64U : remaining;

    HAL_SPI_Transmit(&hspi2, buffer, (uint16_t)(pixels * 2U), HAL_MAX_DELAY);

    remaining -= pixels;
  }

  LCD_CS_High();
}

/*
 * ============================================================
 * Text
 * ============================================================
 */

void LCD_SetCursor(uint16_t x, uint16_t y) {
  cursor_x = x;
  cursor_y = y;
}

void LCD_SetTextColor(uint16_t color) { text_color = color; }

void LCD_SetTextBackground(uint16_t color) { text_background = color; }

void LCD_SetTextSize(uint8_t size) {
  if (size == 0U) {
    size = 1U;
  }

  text_size = size;
}

/*
 * ============================================================
 * 5x7 font
 * ============================================================
 */

static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00},
    {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00},
    {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08},
    {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},

    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},

    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E},
    {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41},
    {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F},
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},

    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E},
    {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07},
    {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00},
    {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},

    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18},
    {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00},
    {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},

    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C},
    {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C},
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00},
    {0x08, 0x04, 0x08, 0x10, 0x08}};

/*
 * ============================================================
 * Character drawing
 * ============================================================
 */

void LCD_WriteChar(char c) {
  if (c < 0x20 || c > 0x7E) {
    return;
  }

  const uint8_t *glyph = font5x7[(uint8_t)c - 0x20U];

  uint16_t width = 6U * text_size;

  uint16_t height = 7U * text_size;

  static uint8_t pixel_buffer[6U * 8U * 7U * 8U * 2U];

  if (width > 48U || height > 56U) {
    return;
  }

  if (cursor_x >= LCD_WIDTH || cursor_y >= LCD_HEIGHT) {
    return;
  }

  uint16_t actual_width = width;

  uint16_t actual_height = height;

  if (cursor_x + actual_width > LCD_WIDTH) {
    actual_width = LCD_WIDTH - cursor_x;
  }

  if (cursor_y + actual_height > LCD_HEIGHT) {
    actual_height = LCD_HEIGHT - cursor_y;
  }

  uint32_t buffer_index = 0U;

  uint8_t color_high = (uint8_t)(text_color >> 8);

  uint8_t color_low = (uint8_t)(text_color & 0xFFU);

  uint8_t background_high = (uint8_t)(text_background >> 8);

  uint8_t background_low = (uint8_t)(text_background & 0xFFU);

  for (uint16_t y = 0U; y < actual_height; y++) {
    uint16_t source_y = y / text_size;

    for (uint16_t x = 0U; x < actual_width; x++) {
      uint16_t source_x = x / text_size;

      bool pixel_on = false;

      if (source_x < 5U && source_y < 7U) {
        pixel_on = (glyph[source_x] & (1U << source_y)) != 0U;
      }

      if (pixel_on) {
        pixel_buffer[buffer_index++] = color_high;

        pixel_buffer[buffer_index++] = color_low;
      } else {
        pixel_buffer[buffer_index++] = background_high;

        pixel_buffer[buffer_index++] = background_low;
      }
    }
  }

  LCD_SetAddressWindow(cursor_x, cursor_y, cursor_x + actual_width - 1U,
                       cursor_y + actual_height - 1U);

  LCD_WriteData(pixel_buffer, (uint16_t)buffer_index);

  cursor_x += width;
}

/*
 * ============================================================
 * String output
 * ============================================================
 */

void LCD_Print(const char *str) {
  if (str == NULL) {
    return;
  }

  while (*str) {
    if (*str == '\n') {
      cursor_x = 0U;

      cursor_y += 8U * text_size;
    } else {
      LCD_WriteChar(*str);
    }

    str++;
  }
}

/*
 * ============================================================
 * Application display requests
 * ============================================================
 */

/**
 * @brief Request a PPS UTC time update.
 *
 * Safe to call from an interrupt.
 */
void LCD_RequestTimeUpdate(const GNSS_DateTime_t *datetime) {
  if (datetime == NULL) {
    return;
  }

  __disable_irq();

  display_datetime = *datetime;

  time_update_pending = true;

  __enable_irq();
}

/**
 * @brief Request an impulse indication.
 *
 * Safe to call from an interrupt.
 *
 * The actual SPI display operation is deferred until
 * LCD_Process().
 */
void LCD_RequestImpulse(char channel) {
  if (channel != '1' && channel != '2') {
    return;
  }

  __disable_irq();

  impulse_channel = channel;

  impulse_start_time = HAL_GetTick();

  impulse_pending = true;

  __enable_irq();
}

/*
 * ============================================================
 * Display rendering
 * ============================================================
 */

/**
 * @brief Draw the current PPS-disciplined UTC time.
 */
static void LCD_DrawTimeScreen(const GNSS_DateTime_t *datetime) {
  char display_string[32];

  uint8_t pos = 0U;

  /*
   * UTC:
   */
  display_string[pos++] = 'U';
  display_string[pos++] = 'T';
  display_string[pos++] = 'C';
  display_string[pos++] = ':';

  display_string[pos++] = '\n';

  /*
   * DD/MM/YYYY
   */
  /* Date: DD/MM/YYYY */

  display_string[pos++] = (char)('0' + (datetime->date.day / 10U));

  display_string[pos++] = (char)('0' + (datetime->date.day % 10U));

  display_string[pos++] = '/';

  display_string[pos++] = (char)('0' + (datetime->date.month / 10U));

  display_string[pos++] = (char)('0' + (datetime->date.month % 10U));

  display_string[pos++] = '/';

  display_string[pos++] = (char)('0' + ((datetime->date.year / 1000U) % 10U));

  display_string[pos++] = (char)('0' + ((datetime->date.year / 100U) % 10U));

  display_string[pos++] = (char)('0' + ((datetime->date.year / 10U) % 10U));

  display_string[pos++] = (char)('0' + (datetime->date.year % 10U));

  display_string[pos++] = '\n';

  /*
   * HH:MM:SS
   */
  display_string[pos++] = '0' + (datetime->time.hours / 10U);

  display_string[pos++] = '0' + (datetime->time.hours % 10U);

  display_string[pos++] = ':';

  display_string[pos++] = '0' + (datetime->time.minutes / 10U);

  display_string[pos++] = '0' + (datetime->time.minutes % 10U);

  display_string[pos++] = ':';

  display_string[pos++] = '0' + (datetime->time.seconds / 10U);

  display_string[pos++] = '0' + (datetime->time.seconds % 10U);

  display_string[pos] = '\0';

  LCD_SetTextColor(LCD_CYAN);

  LCD_SetTextBackground(LCD_BLACK);

  LCD_SetTextSize(2U);

  LCD_SetCursor(10U, 20U);

  LCD_Print(display_string);
}

/**
 * @brief Draw the one-second impulse indication.
 */
static void LCD_DrawImpulseScreen(char channel) {
  char display_string[32];

  display_string[0] = 'I';
  display_string[1] = 'M';
  display_string[2] = 'P';
  display_string[3] = 'U';
  display_string[4] = 'L';
  display_string[5] = 'S';
  display_string[6] = 'E';
  display_string[7] = '\n';

  display_string[8] = 'C';
  display_string[9] = 'H';
  display_string[10] = 'A';
  display_string[11] = 'N';
  display_string[12] = 'N';
  display_string[13] = 'E';
  display_string[14] = 'L';
  display_string[15] = ' ';
  display_string[16] = channel;
  display_string[17] = '\0';

  LCD_SetTextColor(LCD_YELLOW);

  LCD_SetTextBackground(LCD_BLACK);

  LCD_SetTextSize(2U);

  /*
   * Center the text approximately.
   */
  LCD_SetCursor(10U, 40U);

  LCD_Print(display_string);
}

/*
 * ============================================================
 * Main LCD processing
 * ============================================================
 */

/**
 * @brief Process pending LCD updates.
 *
 * This function should be called continuously from the main
 * application loop.
 *
 * Example:
 *
 *     while (1)
 *     {
 *         LCD_Process();
 *         ...
 *     }
 */
void LCD_Process(void) {
  bool process_impulse = false;
  bool process_time = false;

  char channel = '\0';

  GNSS_DateTime_t datetime;

  /*
   * ========================================================
   * Check for a new impulse
   * ========================================================
   */

  __disable_irq();

  if (impulse_pending) {
    impulse_pending = false;

    channel = impulse_channel;

    process_impulse = true;
  }

  /*
   * Copy the latest UTC time.
   */
  datetime = display_datetime;

  /*
   * ========================================================
   * Check whether impulse display has expired
   * ========================================================
   */

  bool impulse_active = (current_screen == LCD_SCREEN_IMPULSE);

  uint32_t impulse_time = impulse_start_time;

  __enable_irq();

  /*
   * New impulse takes priority over everything.
   */
  if (process_impulse) {
    /*
     * Start/restart the one-second indication.
     */
    LCD_FillScreen(LCD_BLACK);

    LCD_DrawImpulseScreen(channel);

    current_screen = LCD_SCREEN_IMPULSE;

    return;
  }

  /*
   * ========================================================
   * Impulse timeout
   * ========================================================
   */

  if (impulse_active) {
    uint32_t now = HAL_GetTick();

    if ((now - impulse_time) >= LCD_IMPULSE_DISPLAY_MS) {
      /*
       * One second has elapsed.
       *
       * Return to the latest PPS UTC time.
       */
      LCD_FillScreen(LCD_BLACK);

      LCD_DrawTimeScreen(&datetime);

      current_screen = LCD_SCREEN_TIME;

      return;
    }

    /*
     * Still displaying the impulse.
     */
    return;
  }

  /*
   * ========================================================
   * Normal UTC time update
   * ========================================================
   */

  __disable_irq();

  if (time_update_pending) {
    time_update_pending = false;

    datetime = display_datetime;

    process_time = true;
  }

  __enable_irq();

  if (process_time) {
    LCD_FillScreen(LCD_BLACK);

    LCD_DrawTimeScreen(&datetime);

    current_screen = LCD_SCREEN_TIME;
  }
}