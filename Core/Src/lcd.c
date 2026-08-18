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

extern SPI_HandleTypeDef hspi2;


/* --------------------------------------------------------------------------
 * ST7735 pins
 * -------------------------------------------------------------------------- */

#define LCD_CS_GPIO_Port    GPIOC
#define LCD_CS_Pin          GPIO_PIN_9

#define LCD_DC_GPIO_Port    GPIOC
#define LCD_DC_Pin          GPIO_PIN_8

#define LCD_RST_GPIO_Port   GPIOC
#define LCD_RST_Pin         GPIO_PIN_7


/* --------------------------------------------------------------------------
 * ST7735 commands
 * -------------------------------------------------------------------------- */

#define ST7735_SWRESET      0x01
#define ST7735_SLPOUT       0x11
#define ST7735_COLMOD       0x3A
#define ST7735_MADCTL       0x36
#define ST7735_CASET        0x2A
#define ST7735_RASET        0x2B
#define ST7735_RAMWR        0x2C
#define ST7735_DISPON       0x29
#define ST7735_INVOFF       0x20


/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static uint16_t cursor_x = 0;
static uint16_t cursor_y = 0;

static uint16_t text_color = LCD_WHITE;
static uint16_t text_background = LCD_BLACK;

static uint8_t text_size = 1;

static volatile GNSS_DateTime_t display_datetime = {0};
static volatile bool time_update_pending = false;


/* --------------------------------------------------------------------------
 * GPIO
 * -------------------------------------------------------------------------- */

static void LCD_CS_Low(void)
{
    HAL_GPIO_WritePin(
        LCD_CS_GPIO_Port,
        LCD_CS_Pin,
        GPIO_PIN_RESET
    );
}


static void LCD_CS_High(void)
{
    HAL_GPIO_WritePin(
        LCD_CS_GPIO_Port,
        LCD_CS_Pin,
        GPIO_PIN_SET
    );
}


static void LCD_DC_Low(void)
{
    HAL_GPIO_WritePin(
        LCD_DC_GPIO_Port,
        LCD_DC_Pin,
        GPIO_PIN_RESET
    );
}


static void LCD_DC_High(void)
{
    HAL_GPIO_WritePin(
        LCD_DC_GPIO_Port,
        LCD_DC_Pin,
        GPIO_PIN_SET
    );
}


static void LCD_Reset(void)
{
    HAL_GPIO_WritePin(
        LCD_RST_GPIO_Port,
        LCD_RST_Pin,
        GPIO_PIN_RESET
    );

    HAL_Delay(20);

    HAL_GPIO_WritePin(
        LCD_RST_GPIO_Port,
        LCD_RST_Pin,
        GPIO_PIN_SET
    );

    HAL_Delay(120);
}


/* --------------------------------------------------------------------------
 * SPI
 * -------------------------------------------------------------------------- */

static void LCD_WriteCommand(uint8_t command)
{
    LCD_CS_Low();
    LCD_DC_Low();

    HAL_SPI_Transmit(
        &hspi2,
        &command,
        1,
        HAL_MAX_DELAY
    );

    LCD_CS_High();
}


static void LCD_WriteData(
    uint8_t *data,
    uint16_t length)
{
    LCD_CS_Low();
    LCD_DC_High();

    HAL_SPI_Transmit(
        &hspi2,
        data,
        length,
        HAL_MAX_DELAY
    );

    LCD_CS_High();
}


static void LCD_WriteDataByte(uint8_t data)
{
    LCD_WriteData(&data, 1);
}


/* --------------------------------------------------------------------------
 * ST7735 address window
 * -------------------------------------------------------------------------- */

/**
 * @brief Set the rectangular region of the display that will receive pixels.
 *
 * The next pixel written to the ST7735 is placed at x0,y0 and subsequent
 * pixels proceed across the selected window.
 */
static void LCD_SetAddressWindow(
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1)
{
    uint8_t data[4];

    /* Column address */
    LCD_WriteCommand(ST7735_CASET);

    data[0] = (uint8_t)(x0 >> 8);
    data[1] = (uint8_t)(x0 & 0xFF);
    data[2] = (uint8_t)(x1 >> 8);
    data[3] = (uint8_t)(x1 & 0xFF);

    LCD_WriteData(data, 4);


    /* Row address */
    LCD_WriteCommand(ST7735_RASET);

    data[0] = (uint8_t)(y0 >> 8);
    data[1] = (uint8_t)(y0 & 0xFF);
    data[2] = (uint8_t)(y1 >> 8);
    data[3] = (uint8_t)(y1 & 0xFF);

    LCD_WriteData(data, 4);


    /* Start writing pixels */
    LCD_WriteCommand(ST7735_RAMWR);
}


/* --------------------------------------------------------------------------
 * Initialisation
 * -------------------------------------------------------------------------- */

void LCD_Init(void)
{
    LCD_Reset();


    /* Software reset */
    LCD_WriteCommand(ST7735_SWRESET);

    HAL_Delay(150);


    /* Exit sleep */
    LCD_WriteCommand(ST7735_SLPOUT);

    HAL_Delay(120);


    /*
     * Pixel format:
     *
     * 0x05 = 16-bit RGB565
     */
    LCD_WriteCommand(ST7735_COLMOD);

    uint8_t color_mode = 0x05;

    LCD_WriteDataByte(color_mode);

    HAL_Delay(10);


    /*
     * Memory access control.
     *
     * 0xA0 is the orientation currently used by this display.
     */
    LCD_WriteCommand(ST7735_MADCTL);

    uint8_t madctl = 0xA0;

    LCD_WriteDataByte(madctl);


    /* Disable display inversion */
    LCD_WriteCommand(ST7735_INVOFF);


    /* Display on */
    LCD_WriteCommand(ST7735_DISPON);

    HAL_Delay(100);


    /* Default text state */
    cursor_x = 0;
    cursor_y = 0;

    text_color = LCD_WHITE;
    text_background = LCD_BLACK;

    text_size = 1;
}


/* --------------------------------------------------------------------------
 * Basic drawing
 * -------------------------------------------------------------------------- */

void LCD_DrawPixel(
    uint16_t x,
    uint16_t y,
    uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    LCD_SetAddressWindow(
        x,
        y,
        x,
        y
    );

    uint8_t data[2];

    data[0] = (uint8_t)(color >> 8);
    data[1] = (uint8_t)(color & 0xFF);

    LCD_WriteData(data, 2);
}


/**
 * @brief Fill the entire display with one colour.
 */
void LCD_FillScreen(uint16_t color)
{
    LCD_SetAddressWindow(
        0,
        0,
        LCD_WIDTH - 1,
        LCD_HEIGHT - 1
    );

    uint8_t buffer[128];

    uint8_t high = (uint8_t)(color >> 8);
    uint8_t low  = (uint8_t)(color & 0xFF);

    for (uint16_t i = 0; i < sizeof(buffer); i += 2)
    {
        buffer[i]     = high;
        buffer[i + 1] = low;
    }

    LCD_CS_Low();
    LCD_DC_High();

    /*
     * 128 bytes = 64 RGB565 pixels.
     */
    uint32_t pixel_count =
        (uint32_t)LCD_WIDTH * LCD_HEIGHT;

    uint32_t pixels_sent = 0;

    while (pixels_sent < pixel_count)
    {
        uint32_t remaining =
            pixel_count - pixels_sent;

        uint16_t pixels =
            (remaining >= 64) ? 64 : (uint16_t)remaining;

        HAL_SPI_Transmit(
            &hspi2,
            buffer,
            (uint16_t)(pixels * 2),
            HAL_MAX_DELAY
        );

        pixels_sent += pixels;
    }

    LCD_CS_High();
}


/**
 * @brief Draw a horizontal line efficiently.
 */
void LCD_DrawFastHLine(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    if (width > LCD_WIDTH - x)
        width = LCD_WIDTH - x;

    if (width == 0)
        return;

    LCD_SetAddressWindow(
        x,
        y,
        x + width - 1,
        y
    );

    uint8_t buffer[128];

    uint8_t high = (uint8_t)(color >> 8);
    uint8_t low  = (uint8_t)(color & 0xFF);

    for (uint16_t i = 0; i < sizeof(buffer); i += 2)
    {
        buffer[i]     = high;
        buffer[i + 1] = low;
    }

    LCD_CS_Low();
    LCD_DC_High();

    uint16_t remaining = width;

    while (remaining > 0)
    {
        uint16_t pixels =
            (remaining > 64) ? 64 : remaining;

        HAL_SPI_Transmit(
            &hspi2,
            buffer,
            pixels * 2,
            HAL_MAX_DELAY
        );

        remaining -= pixels;
    }

    LCD_CS_High();
}


/* --------------------------------------------------------------------------
 * Text
 * -------------------------------------------------------------------------- */

void LCD_SetCursor(
    uint16_t x,
    uint16_t y)
{
    cursor_x = x;
    cursor_y = y;
}


void LCD_SetTextColor(uint16_t color)
{
    text_color = color;
}


void LCD_SetTextSize(uint8_t size)
{
    if (size == 0)
        size = 1;

    text_size = size;
}


/**
 * @brief Set the background colour used when drawing text.
 *
 * The background is important because characters are redrawn directly
 * over previous characters. Pixels belonging to the glyph are drawn
 * using text_color and empty glyph pixels are drawn using this colour.
 */
void LCD_SetTextBackground(uint16_t color)
{
    text_background = color;
}


/* --------------------------------------------------------------------------
 * 5x7 font
 * -------------------------------------------------------------------------- */

static const uint8_t font5x7[95][5] = {

    /* 0x20 ' ' */
    {0x00,0x00,0x00,0x00,0x00},

    /* 0x21 '!' */
    {0x00,0x00,0x5F,0x00,0x00},

    /* 0x22 '"' */
    {0x00,0x07,0x00,0x07,0x00},

    /* 0x23 '#' */
    {0x14,0x7F,0x14,0x7F,0x14},

    /* 0x24 '$' */
    {0x24,0x2A,0x7F,0x2A,0x12},

    /* 0x25 '%' */
    {0x23,0x13,0x08,0x64,0x62},

    /* 0x26 '&' */
    {0x36,0x49,0x55,0x22,0x50},

    /* 0x27 '\'' */
    {0x00,0x05,0x03,0x00,0x00},

    /* 0x28 '(' */
    {0x00,0x1C,0x22,0x41,0x00},

    /* 0x29 ')' */
    {0x00,0x41,0x22,0x1C,0x00},

    /* 0x2A '*' */
    {0x14,0x08,0x3E,0x08,0x14},

    /* 0x2B '+' */
    {0x08,0x08,0x3E,0x08,0x08},

    /* 0x2C ',' */
    {0x00,0x50,0x30,0x00,0x00},

    /* 0x2D '-' */
    {0x08,0x08,0x08,0x08,0x08},

    /* 0x2E '.' */
    {0x00,0x60,0x60,0x00,0x00},

    /* 0x2F '/' */
    {0x20,0x10,0x08,0x04,0x02},

    /* 0x30 '0' */
    {0x3E,0x51,0x49,0x45,0x3E},

    /* 0x31 '1' */
    {0x00,0x42,0x7F,0x40,0x00},

    /* 0x32 '2' */
    {0x42,0x61,0x51,0x49,0x46},

    /* 0x33 '3' */
    {0x21,0x41,0x45,0x4B,0x31},

    /* 0x34 '4' */
    {0x18,0x14,0x12,0x7F,0x10},

    /* 0x35 '5' */
    {0x27,0x45,0x45,0x45,0x39},

    /* 0x36 '6' */
    {0x3C,0x4A,0x49,0x49,0x30},

    /* 0x37 '7' */
    {0x01,0x71,0x09,0x05,0x03},

    /* 0x38 '8' */
    {0x36,0x49,0x49,0x49,0x36},

    /* 0x39 '9' */
    {0x06,0x49,0x49,0x29,0x1E},

    /* 0x3A ':' */
    {0x00,0x36,0x36,0x00,0x00},

    /* 0x3B ';' */
    {0x00,0x56,0x36,0x00,0x00},

    /* 0x3C '<' */
    {0x08,0x14,0x22,0x41,0x00},

    /* 0x3D '=' */
    {0x14,0x14,0x14,0x14,0x14},

    /* 0x3E '>' */
    {0x00,0x41,0x22,0x14,0x08},

    /* 0x3F '?' */
    {0x02,0x01,0x51,0x09,0x06},

    /* 0x40 '@' */
    {0x32,0x49,0x79,0x41,0x3E},

    /* 0x41 'A' */
    {0x7E,0x11,0x11,0x11,0x7E},

    /* 0x42 'B' */
    {0x7F,0x49,0x49,0x49,0x36},

    /* 0x43 'C' */
    {0x3E,0x41,0x41,0x41,0x22},

    /* 0x44 'D' */
    {0x7F,0x41,0x41,0x22,0x1C},

    /* 0x45 'E' */
    {0x7F,0x49,0x49,0x49,0x41},

    /* 0x46 'F' */
    {0x7F,0x09,0x09,0x09,0x01},

    /* 0x47 'G' */
    {0x3E,0x41,0x49,0x49,0x7A},

    /* 0x48 'H' */
    {0x7F,0x08,0x08,0x08,0x7F},

    /* 0x49 'I' */
    {0x00,0x41,0x7F,0x41,0x00},

    /* 0x4A 'J' */
    {0x20,0x40,0x41,0x3F,0x01},

    /* 0x4B 'K' */
    {0x7F,0x08,0x14,0x22,0x41},

    /* 0x4C 'L' */
    {0x7F,0x40,0x40,0x40,0x40},

    /* 0x4D 'M' */
    {0x7F,0x02,0x0C,0x02,0x7F},

    /* 0x4E 'N' */
    {0x7F,0x04,0x08,0x10,0x7F},

    /* 0x4F 'O' */
    {0x3E,0x41,0x41,0x41,0x3E},

    /* 0x50 'P' */
    {0x7F,0x09,0x09,0x09,0x06},

    /* 0x51 'Q' */
    {0x3E,0x41,0x51,0x21,0x5E},

    /* 0x52 'R' */
    {0x7F,0x09,0x19,0x29,0x46},

    /* 0x53 'S' */
    {0x46,0x49,0x49,0x49,0x31},

    /* 0x54 'T' */
    {0x01,0x01,0x7F,0x01,0x01},

    /* 0x55 'U' */
    {0x3F,0x40,0x40,0x40,0x3F},

    /* 0x56 'V' */
    {0x1F,0x20,0x40,0x20,0x1F},

    /* 0x57 'W' */
    {0x7F,0x20,0x18,0x20,0x7F},

    /* 0x58 'X' */
    {0x63,0x14,0x08,0x14,0x63},

    /* 0x59 'Y' */
    {0x07,0x08,0x70,0x08,0x07},

    /* 0x5A 'Z' */
    {0x61,0x51,0x49,0x45,0x43},

    /* 0x5B '[' */
    {0x00,0x7F,0x41,0x41,0x00},

    /* 0x5C '\' */
    {0x02,0x04,0x08,0x10,0x20},

    /* 0x5D ']' */
    {0x00,0x41,0x41,0x7F,0x00},

    /* 0x5E '^' */
    {0x04,0x02,0x01,0x02,0x04},

    /* 0x5F '_' */
    {0x40,0x40,0x40,0x40,0x40},

    /* 0x60 '`' */
    {0x00,0x01,0x02,0x04,0x00},

    /* 0x61 'a' */
    {0x20,0x54,0x54,0x54,0x78},

    /* 0x62 'b' */
    {0x7F,0x48,0x44,0x44,0x38},

    /* 0x63 'c' */
    {0x38,0x44,0x44,0x44,0x20},

    /* 0x64 'd' */
    {0x38,0x44,0x44,0x48,0x7F},

    /* 0x65 'e' */
    {0x38,0x54,0x54,0x54,0x18},

    /* 0x66 'f' */
    {0x08,0x7E,0x09,0x01,0x02},

    /* 0x67 'g' */
    {0x0C,0x52,0x52,0x52,0x3E},

    /* 0x68 'h' */
    {0x7F,0x08,0x04,0x04,0x78},

    /* 0x69 'i' */
    {0x00,0x44,0x7D,0x40,0x00},

    /* 0x6A 'j' */
    {0x20,0x40,0x44,0x3D,0x00},

    /* 0x6B 'k' */
    {0x7F,0x10,0x28,0x44,0x00},

    /* 0x6C 'l' */
    {0x00,0x41,0x7F,0x40,0x00},

    /* 0x6D 'm' */
    {0x7C,0x04,0x18,0x04,0x78},

    /* 0x6E 'n' */
    {0x7C,0x08,0x04,0x04,0x78},

    /* 0x6F 'o' */
    {0x38,0x44,0x44,0x44,0x38},

    /* 0x70 'p' */
    {0x7C,0x14,0x14,0x14,0x08},

    /* 0x71 'q' */
    {0x08,0x14,0x14,0x18,0x7C},

    /* 0x72 'r' */
    {0x7C,0x08,0x04,0x04,0x08},

    /* 0x73 's' */
    {0x48,0x54,0x54,0x54,0x20},

    /* 0x74 't' */
    {0x04,0x3F,0x44,0x40,0x20},

    /* 0x75 'u' */
    {0x3C,0x40,0x40,0x20,0x7C},

    /* 0x76 'v' */
    {0x1C,0x20,0x40,0x20,0x1C},

    /* 0x77 'w' */
    {0x3C,0x40,0x30,0x40,0x3C},

    /* 0x78 'x' */
    {0x44,0x28,0x10,0x28,0x44},

    /* 0x79 'y' */
    {0x0C,0x50,0x50,0x50,0x3C},

    /* 0x7A 'z' */
    {0x44,0x64,0x54,0x4C,0x44},

    /* 0x7B '{' */
    {0x00,0x08,0x36,0x41,0x00},

    /* 0x7C '|' */
    {0x00,0x00,0x7F,0x00,0x00},

    /* 0x7D '}' */
    {0x00,0x41,0x36,0x08,0x00},

    /* 0x7E '~' */
    {0x08,0x04,0x08,0x10,0x08}
};


/* --------------------------------------------------------------------------
 * Optimised character drawing
 * -------------------------------------------------------------------------- */

/**
 * @brief Draw one character using a single ST7735 address window.
 *
 * Unlike the previous implementation, this does not call LCD_DrawPixel()
 * for every pixel. The complete character is assembled in RAM and sent
 * to the display in one SPI transaction.
 *
 * The character includes its blank spacing column. This is intentional:
 * when a character is redrawn, the blank column clears any pixels left
 * by the previous character.
 */
void LCD_WriteChar(char c)
{
    if (c < 0x20 || c > 0x7E)
        return;

    const uint8_t *glyph =
        font5x7[(uint8_t)c - 0x20];

    /*
     * Character dimensions:
     *
     *   5 columns of glyph data
     *   + 1 blank spacing column
     *
     * At text_size 2 this becomes:
     *
     *   12 x 14 pixels
     */
    uint16_t width =
        6U * text_size;

    uint16_t height =
        7U * text_size;

    /*
     * Maximum size supported without dynamic allocation.
     *
     * 6 * 8 = 48 pixels wide
     * 7 * 8 = 56 pixels high
     *
     * 48 * 56 * 2 = 5376 bytes.
     */
    static uint8_t pixel_buffer[6 * 8 * 7 * 8 * 2];

    if (width > 48 || height > 56)
        return;

    if (cursor_x >= LCD_WIDTH ||
        cursor_y >= LCD_HEIGHT)
        return;

    uint16_t actual_width = width;
    uint16_t actual_height = height;

    if (cursor_x + actual_width > LCD_WIDTH)
    {
        actual_width =
            LCD_WIDTH - cursor_x;
    }

    if (cursor_y + actual_height > LCD_HEIGHT)
    {
        actual_height =
            LCD_HEIGHT - cursor_y;
    }

    /*
     * Build the complete character image.
     *
     * Both foreground and background pixels are written.
     * Therefore a new character completely replaces the old
     * character underneath it.
     */
    uint32_t buffer_index = 0;

    uint8_t color_high =
        (uint8_t)(text_color >> 8);

    uint8_t color_low =
        (uint8_t)(text_color & 0xFF);

    uint8_t background_high =
        (uint8_t)(text_background >> 8);

    uint8_t background_low =
        (uint8_t)(text_background & 0xFF);

    for (uint16_t y = 0;
         y < actual_height;
         y++)
    {
        uint16_t source_y =
            y / text_size;

        for (uint16_t x = 0;
             x < actual_width;
             x++)
        {
            uint16_t source_x =
                x / text_size;

            bool pixel_on = false;

            /*
             * The sixth column is the spacing column.
             */
            if (source_x < 5 &&
                source_y < 7)
            {
                pixel_on =
                    (glyph[source_x] &
                     (1U << source_y)) != 0;
            }

            if (pixel_on)
            {
                pixel_buffer[buffer_index++] =
                    color_high;

                pixel_buffer[buffer_index++] =
                    color_low;
            }
            else
            {
                pixel_buffer[buffer_index++] =
                    background_high;

                pixel_buffer[buffer_index++] =
                    background_low;
            }
        }
    }

    LCD_SetAddressWindow(
        cursor_x,
        cursor_y,
        cursor_x + actual_width - 1,
        cursor_y + actual_height - 1
    );

    LCD_WriteData(
        pixel_buffer,
        (uint16_t)buffer_index
    );

    /*
     * Advance by the complete character width.
     */
    cursor_x += width;
}


/**
 * @brief Print a null-terminated string.
 */
void LCD_Print(const char *str)
{
    if (str == NULL)
        return;

    while (*str)
    {
        if (*str == '\n')
        {
            cursor_x = 0;

            cursor_y +=
                8U * text_size;
        }
        else
        {
            LCD_WriteChar(*str);
        }

        str++;
    }
}


/* --------------------------------------------------------------------------
 * GNSS time display
 * -------------------------------------------------------------------------- */

/**
 * @brief Request an update of the displayed UTC date and time.
 *
 * Copies the latest GNSS date/time into the LCD module and marks an
 * update as pending. The actual display operation is performed by
 * LCD_Process() so that blocking SPI transfers never occur inside
 * the GNSS receive interrupt/callback.
 *
 * @param datetime Pointer to the latest GNSS UTC date/time.
 */
void LCD_RequestTimeUpdate(
    const GNSS_DateTime_t *datetime)
{
    if (datetime == NULL)
        return;

    __disable_irq();

    display_datetime = *datetime;
    time_update_pending = true;

    __enable_irq();
}


/**
 * @brief Process a pending GNSS date/time display update.
 *
 * The display is updated only when GNSS has supplied new time data.
 * The previous implementation cleared the entire LCD before drawing
 * the new time. That required transferring every pixel on the display.
 *
 * Instead, each character is now drawn as a complete rectangle using
 * the LCD background colour for blank pixels. Consequently a new
 * character completely replaces the old character without requiring
 * a full-screen clear.
 */
void LCD_Process(void)
{
    GNSS_DateTime_t datetime;

    __disable_irq();

    if (!time_update_pending)
    {
        __enable_irq();
        return;
    }

    datetime = display_datetime;

    time_update_pending = false;

    __enable_irq();


    /*
     * Construct the display text.
     *
     * Layout:
     *
     *     UTC:
     *     DD/MM/YYYY
     *     HH:MM:SS
     */
    char display_string[32];

    uint8_t pos = 0;


    /* "UTC:" */

    display_string[pos++] = 'U';
    display_string[pos++] = 'T';
    display_string[pos++] = 'C';
    display_string[pos++] = ':';

    display_string[pos++] = '\n';


    /* Date: DD/MM/YYYY */

    display_string[pos++] =
        '0' + (datetime.date.day / 10);

    display_string[pos++] =
        '0' + (datetime.date.day % 10);

    display_string[pos++] = '/';

    display_string[pos++] =
        '0' + (datetime.date.month / 10);

    display_string[pos++] =
        '0' + (datetime.date.month % 10);

    display_string[pos++] = '/';

    display_string[pos++] =
        '0' + ((datetime.date.year / 1000) % 10);

    display_string[pos++] =
        '0' + ((datetime.date.year / 100) % 10);

    display_string[pos++] =
        '0' + ((datetime.date.year / 10) % 10);

    display_string[pos++] =
        '0' + (datetime.date.year % 10);

    display_string[pos++] = '\n';


    /* Time: HH:MM:SS */

    display_string[pos++] =
        '0' + (datetime.time.hours / 10);

    display_string[pos++] =
        '0' + (datetime.time.hours % 10);

    display_string[pos++] = ':';

    display_string[pos++] =
        '0' + (datetime.time.minutes / 10);

    display_string[pos++] =
        '0' + (datetime.time.minutes % 10);

    display_string[pos++] = ':';

    display_string[pos++] =
        '0' + (datetime.time.seconds / 10);

    display_string[pos++] =
        '0' + (datetime.time.seconds % 10);

    display_string[pos] = '\0';


    /*
     * Configure text rendering.
     */
    LCD_SetTextColor(LCD_CYAN);

    LCD_SetTextBackground(LCD_BLACK);

    LCD_SetTextSize(2);

    LCD_SetCursor(25, 20);


    /*
     * No LCD_FillScreen() here.
     *
     * Every character writes its own background pixels, so the old
     * text is automatically erased as the new text is drawn.
     */
    LCD_Print(display_string);
}