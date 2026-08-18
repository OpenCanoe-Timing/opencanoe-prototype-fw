/**
 * @file lcd.c
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief LCD Management.
 *
 * @copyright
 * Copyright (c) 2026 Alexander Ellul.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "lcd.h"

#include <stdbool.h>
#include <stdint.h>

extern SPI_HandleTypeDef hspi2;

/* --------------------------------------------------------------------------
 * ST7735 pins
 * -------------------------------------------------------------------------- */

#define LCD_CS_GPIO_Port GPIOC
#define LCD_CS_Pin GPIO_PIN_9

#define LCD_DC_GPIO_Port GPIOC
#define LCD_DC_Pin GPIO_PIN_8

#define LCD_RST_GPIO_Port GPIOC
#define LCD_RST_Pin GPIO_PIN_7

/* --------------------------------------------------------------------------
 * ST7735 commands
 * -------------------------------------------------------------------------- */

#define ST7735_SWRESET 0x01
#define ST7735_SLPOUT 0x11
#define ST7735_COLMOD 0x3A
#define ST7735_MADCTL 0x36
#define ST7735_CASET 0x2A
#define ST7735_RASET 0x2B
#define ST7735_RAMWR 0x2C
#define ST7735_DISPON 0x29
#define ST7735_INVOFF 0x20

/* --------------------------------------------------------------------------
 * Application display state
 * -------------------------------------------------------------------------- */

#define LCD_IMPULSE_DISPLAY_MS 1000U

static uint16_t cursor_x = 0U;
static uint16_t cursor_y = 0U;

static uint16_t text_color = LCD_WHITE;
static uint16_t text_background = LCD_BLACK;

static uint8_t text_size = 1U;

static volatile GNSS_DateTime_t display_datetime = {0};

static volatile bool time_update_pending = false;

/*
 * False until the first valid PPS has established UTC.
 */
static volatile bool gnss_pps_locked = false;

/*
 * Timing impulse display state.
 */
static volatile bool impulse_pending = false;

static volatile bool impulse_displayed = false;

static volatile char impulse_channel = '\0';

static volatile char displayed_impulse_channel = '\0';

static volatile uint32_t impulse_start_tick = 0U;

/*
 * Used to identify a new impulse event.
 *
 * This increments every time LCD_RequestImpulse() is called.
 * It prevents LCD_Process() from accidentally clearing or
 * overwriting a newer impulse with an older one.
 */
static volatile uint32_t impulse_sequence = 0U;

/*
 * Sequence number of the impulse currently displayed.
 */
static uint32_t displayed_impulse_sequence = 0U;

/*
 * Used to ensure the initial lock message is drawn.
 */
static bool display_initialised = false;

/* --------------------------------------------------------------------------
 * GPIO
 * -------------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------------
 * SPI
 * -------------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------------
 * Address window
 * -------------------------------------------------------------------------- */

static void LCD_SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1,
                                 uint16_t y1) {

  uint8_t data[4];

  LCD_WriteCommand(ST7735_CASET);

  data[0] = (uint8_t)(x0 >> 8);
  data[1] = (uint8_t)(x0 & 0xFFU);
  data[2] = (uint8_t)(x1 >> 8);
  data[3] = (uint8_t)(x1 & 0xFFU);

  LCD_WriteData(data, 4U);

  LCD_WriteCommand(ST7735_RASET);

  data[0] = (uint8_t)(y0 >> 8);
  data[1] = (uint8_t)(y0 & 0xFFU);
  data[2] = (uint8_t)(y1 >> 8);
  data[3] = (uint8_t)(y1 & 0xFFU);

  LCD_WriteData(data, 4U);

  LCD_WriteCommand(ST7735_RAMWR);
}

/* --------------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------------- */

void LCD_Init(void) {
  LCD_Reset();

  LCD_WriteCommand(ST7735_SWRESET);

  HAL_Delay(150U);

  LCD_WriteCommand(ST7735_SLPOUT);

  HAL_Delay(120U);

  LCD_WriteCommand(ST7735_COLMOD);

  uint8_t color_mode = 0x05;

  LCD_WriteDataByte(color_mode);

  HAL_Delay(10U);

  LCD_WriteCommand(ST7735_MADCTL);

  uint8_t madctl = 0xA0;

  LCD_WriteDataByte(madctl);

  LCD_WriteCommand(ST7735_INVOFF);

  LCD_WriteCommand(ST7735_DISPON);

  HAL_Delay(100U);

  cursor_x = 0U;
  cursor_y = 0U;

  text_color = LCD_WHITE;
  text_background = LCD_BLACK;

  text_size = 1U;

  /*
   * Start in the GNSS-lock screen.
   */
  gnss_pps_locked = false;

  time_update_pending = false;

  impulse_pending = false;

  impulse_displayed = false;

  impulse_channel = '\0';

  displayed_impulse_channel = '\0';

  impulse_start_tick = 0U;

  impulse_sequence = 0U;

  displayed_impulse_sequence = 0U;

  display_initialised = false;
}

/* --------------------------------------------------------------------------
 * Basic drawing
 * -------------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------------
 * Text
 * -------------------------------------------------------------------------- */

void LCD_SetCursor(uint16_t x, uint16_t y) {

  cursor_x = x;
  cursor_y = y;
}

void LCD_SetTextColor(uint16_t color) { text_color = color; }

void LCD_SetTextSize(uint8_t size) {

  if (size == 0U) {
    size = 1U;
  }

  text_size = size;
}

void LCD_SetTextBackground(uint16_t color) { text_background = color; }

/* --------------------------------------------------------------------------
 * 5x7 font
 * -------------------------------------------------------------------------- */

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

/* --------------------------------------------------------------------------
 * Character drawing
 * -------------------------------------------------------------------------- */

void LCD_WriteChar(char c) {

  if (c < 0x20 || c > 0x7E) {
    return;
  }

  const uint8_t *glyph = font5x7[(uint8_t)c - 0x20U];

  uint16_t width = 6U * text_size;

  uint16_t height = 7U * text_size;

  static uint8_t pixel_buffer[6 * 8 * 7 * 8 * 2];

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

void LCD_Print(const char *str) {

  if (str == NULL) {
    return;
  }

  while (*str != '\0') {

    if (*str == '\n') {

      cursor_x = 0U;

      cursor_y += 8U * text_size;

    } else {

      LCD_WriteChar(*str);
    }

    str++;
  }
}

/* --------------------------------------------------------------------------
 * GNSS warning symbol
 * -------------------------------------------------------------------------- */

/**
 * @brief Draw a small warning triangle with an exclamation mark.
 *
 * The symbol is intentionally drawn directly rather than being added to
 * the font table. This keeps the existing text rendering unchanged.
 */
static void LCD_DrawWarningSymbol(uint16_t center_x, uint16_t top_y,
                                  uint16_t size, uint16_t color) {

  if (size < 6U) {
    return;
  }

  uint16_t bottom_y = top_y + size;

  uint16_t left_x = center_x - (size / 2U);

  uint16_t right_x = center_x + (size / 2U);

  /*
   * Left and right diagonal edges.
   */
  for (uint16_t i = 0U; i <= size; i++) {

    uint16_t y = top_y + i;

    /*
     * Correctly form the diagonal based on the current height.
     */
    uint16_t left = center_x - (uint16_t)((size / 2U) * i / size);

    uint16_t right = center_x + (uint16_t)((size / 2U) * i / size);

    if (left < LCD_WIDTH && y < LCD_HEIGHT) {
      LCD_DrawPixel(left, y, color);
    }

    if (right < LCD_WIDTH && y < LCD_HEIGHT) {
      LCD_DrawPixel(right, y, color);
    }
  }

  /*
   * Bottom edge.
   */
  if (bottom_y < LCD_HEIGHT) {

    for (uint16_t x = left_x; x <= right_x; x++) {

      if (x < LCD_WIDTH) {
        LCD_DrawPixel(x, bottom_y, color);
      }
    }
  }

  /*
   * Exclamation mark.
   */
  uint16_t exclamation_x = center_x;

  uint16_t exclamation_top = top_y + size / 3U;

  uint16_t exclamation_bottom = top_y + (size * 2U) / 3U;

  for (uint16_t y = exclamation_top; y <= exclamation_bottom; y++) {

    if (exclamation_x < LCD_WIDTH && y < LCD_HEIGHT) {

      LCD_DrawPixel(exclamation_x, y, color);
    }
  }

  /*
   * Exclamation dot.
   */
  uint16_t dot_y = top_y + (size * 4U) / 5U;

  if (exclamation_x < LCD_WIDTH && dot_y < LCD_HEIGHT) {

    LCD_DrawPixel(exclamation_x, dot_y, color);
  }
}

/* --------------------------------------------------------------------------
 * GNSS state
 * -------------------------------------------------------------------------- */

void LCD_SetGNSSLock(bool locked) {

  __disable_irq();

  gnss_pps_locked = locked;

  /*
   * Force the display to redraw whenever the lock state changes.
   */
  display_initialised = false;

  time_update_pending = true;

  __enable_irq();
}

void LCD_RequestTimeUpdate(const GNSS_DateTime_t *datetime) {

  if (datetime == NULL) {
    return;
  }

  __disable_irq();

  display_datetime = *datetime;

  time_update_pending = true;

  __enable_irq();
}

/* --------------------------------------------------------------------------
 * Timing impulse
 * -------------------------------------------------------------------------- */

void LCD_RequestImpulse(char channel) {

  if (channel != '1' && channel != '2') {

    return;
  }

  __disable_irq();

  /*
   * Replace the current impulse with the new one.
   *
   * This means a new impulse immediately becomes
   * the active display event.
   */
  impulse_channel = channel;

  impulse_start_tick = HAL_GetTick();

  impulse_sequence++;

  impulse_pending = true;

  /*
   * Force LCD_Process() to redraw the impulse.
   */
  impulse_displayed = false;

  __enable_irq();
}

/* --------------------------------------------------------------------------
 * Display processing
 * -------------------------------------------------------------------------- */

void LCD_Process(void) {

  GNSS_DateTime_t datetime;

  bool locked;
  bool impulse;
  bool update;

  char channel;

  uint32_t impulse_tick;
  uint32_t sequence;

  __disable_irq();

  datetime = display_datetime;

  locked = gnss_pps_locked;

  impulse = impulse_pending;

  channel = impulse_channel;

  impulse_tick = impulse_start_tick;

  sequence = impulse_sequence;

  update = time_update_pending;

  time_update_pending = false;

  __enable_irq();

  /*
   * ============================================================
   * GNSS NOT LOCKED
   * ============================================================
   */

  if (!locked) {

    if (!display_initialised || update) {

      LCD_FillScreen(LCD_BLACK);

      LCD_SetTextColor(LCD_RED);

      LCD_SetTextBackground(LCD_BLACK);

      /*
       * Keep the existing text size exactly as requested.
       */
      LCD_SetTextSize(2U);

      /*
       * Warning symbol.
       */
      LCD_DrawWarningSymbol(80U, 5U, 24U, LCD_RED);

      /*
       * Warning message.
       */
      LCD_SetCursor(8U, 45U);

      LCD_Print("GNSS lock\n"
                "not achieved!");

      display_initialised = true;
    }

    return;
  }

  /*
   * ============================================================
   * TIMING IMPULSE INDICATION
   * ============================================================
   */

  if (impulse) {

    uint32_t now = HAL_GetTick();

    /*
     * Unsigned subtraction handles HAL_GetTick()
     * rollover correctly.
     */
    uint32_t elapsed = (uint32_t)(now - impulse_tick);

    /*
     * Check that the impulse we copied above is
     * still the current impulse.
     *
     * If a new impulse arrived while we were preparing
     * the display, abandon this one and let the next
     * LCD_Process() iteration handle the new impulse.
     */
    __disable_irq();

    bool still_current =
        (impulse_sequence == sequence) && (impulse_channel == channel) &&
        (impulse_start_tick == impulse_tick) && impulse_pending;

    __enable_irq();

    if (!still_current) {
      /*
       * A newer impulse has arrived.
       *
       * Do not draw the stale impulse.
       */
      return;
    }

    if (elapsed < LCD_IMPULSE_DISPLAY_MS) {

      /*
       * Draw only when necessary.
       *
       * This prevents flickering from repeatedly
       * filling the screen.
       *
       * The sequence number also means that a new
       * impulse on the same channel is recognised
       * as a new event.
       */
      if (!impulse_displayed || displayed_impulse_channel != channel ||
          displayed_impulse_sequence != sequence) {

        LCD_FillScreen(LCD_BLACK);

        LCD_SetTextColor(LCD_YELLOW);

        LCD_SetTextBackground(LCD_BLACK);

        LCD_SetTextSize(2U);

        /*
         * 160x128 display.
         */
        LCD_SetCursor(20U, 35U);

        LCD_Print("IMPULSE ON\n");

        LCD_SetCursor(26U, 60U);

        if (channel == '1') {
          LCD_Print("CHANNEL 1");
        } else {
          LCD_Print("CHANNEL 2");
        }

        /*
         * Remember exactly which impulse is visible.
         */
        displayed_impulse_channel = channel;

        displayed_impulse_sequence = sequence;

        impulse_displayed = true;
      }

      return;
    }

    /*
     * ========================================================
     * One second has elapsed.
     * ========================================================
     */

    __disable_irq();

    /*
     * Only clear the impulse if it is still the
     * exact impulse that we were timing.
     */
    if (impulse_pending && impulse_sequence == sequence &&
        impulse_channel == channel && impulse_start_tick == impulse_tick) {

      impulse_pending = false;
    }

    __enable_irq();

    impulse_displayed = false;

    displayed_impulse_channel = '\0';

    displayed_impulse_sequence = 0U;

    /*
     * Fall through to redraw UTC.
     */
    update = true;
  }

  /*
   * ============================================================
   * UTC DISPLAY
   * ============================================================
   */

  if (!update && !display_initialised) {

    update = true;
  }

  if (!update) {
    return;
  }

  LCD_FillScreen(LCD_BLACK);

  LCD_SetTextColor(LCD_CYAN);

  LCD_SetTextBackground(LCD_BLACK);

  LCD_SetTextSize(2U);

  LCD_SetCursor(25U, 20U);

  /*
   * UTC:
   */
  LCD_Print("UTC:\n");

  /*
   * Date.
   *
   * Explicitly construct exactly two digits
   * for day and month.
   */
  char date_string[16];

  date_string[0] = (char)('0' + (datetime.date.day / 10U));

  date_string[1] = (char)('0' + (datetime.date.day % 10U));

  date_string[2] = '/';

  date_string[3] = (char)('0' + (datetime.date.month / 10U));

  date_string[4] = (char)('0' + (datetime.date.month % 10U));

  date_string[5] = '/';

  date_string[6] = (char)('0' + ((datetime.date.year / 1000U) % 10U));

  date_string[7] = (char)('0' + ((datetime.date.year / 100U) % 10U));

  date_string[8] = (char)('0' + ((datetime.date.year / 10U) % 10U));

  date_string[9] = (char)('0' + (datetime.date.year % 10U));

  date_string[10] = '\0';

  LCD_Print(date_string);

  LCD_Print("\n");

  /*
   * Time.
   */
  char time_string[16];

  time_string[0] = (char)('0' + (datetime.time.hours / 10U));

  time_string[1] = (char)('0' + (datetime.time.hours % 10U));

  time_string[2] = ':';

  time_string[3] = (char)('0' + (datetime.time.minutes / 10U));

  time_string[4] = (char)('0' + (datetime.time.minutes % 10U));

  time_string[5] = ':';

  time_string[6] = (char)('0' + (datetime.time.seconds / 10U));

  time_string[7] = (char)('0' + (datetime.time.seconds % 10U));

  time_string[8] = '\0';

  LCD_Print(time_string);

  /*
   * We are no longer displaying an impulse.
   */
  impulse_displayed = false;

  displayed_impulse_channel = '\0';

  displayed_impulse_sequence = 0U;

  display_initialised = true;
}