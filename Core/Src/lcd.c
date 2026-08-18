#include "lcd.h"

/*
 * Change this if you're using another SPI peripheral.
 */
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
#define ST7735_INVON        0x21


/* --------------------------------------------------------------------------
 * Internal state
 * -------------------------------------------------------------------------- */

static uint16_t cursor_x = 0;
static uint16_t cursor_y = 0;
static uint16_t text_color = LCD_WHITE;
static uint8_t text_size = 1;


/* --------------------------------------------------------------------------
 * GPIO
 * -------------------------------------------------------------------------- */

static void LCD_CS_Low(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
}

static void LCD_CS_High(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

static void LCD_DC_Low(void)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
}

static void LCD_DC_High(void)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
}

static void LCD_Reset(void)
{
    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
    HAL_Delay(20);

    HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
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

static void LCD_WriteData(uint8_t *data, uint16_t length)
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

static void LCD_SetAddressWindow(
    uint16_t x0,
    uint16_t y0,
    uint16_t x1,
    uint16_t y1)
{
    uint8_t data[4];

    /* Column address */
    LCD_WriteCommand(ST7735_CASET);

    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;

    LCD_WriteData(data, 4);


    /* Row address */
    LCD_WriteCommand(ST7735_RASET);

    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;

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
     * 0xC8 is a common configuration for
     * 128x160 ST7735 modules.
     */
    LCD_WriteCommand(ST7735_MADCTL);

    uint8_t madctl = 0xA8;
    LCD_WriteDataByte(madctl);


    /*
     * Inversion is commonly enabled on ST7735 modules.
     */
    LCD_WriteCommand(ST7735_INVON);


    /* Display on */
    LCD_WriteCommand(ST7735_DISPON);
    HAL_Delay(100);


    /* Default text state */
    cursor_x = 0;
    cursor_y = 0;
    text_color = LCD_WHITE;
    text_size = 1;
}


/* --------------------------------------------------------------------------
 * Drawing
 * -------------------------------------------------------------------------- */

void LCD_DrawPixel(
    uint16_t x,
    uint16_t y,
    uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    LCD_SetAddressWindow(x, y, x, y);

    uint8_t data[2];

    data[0] = color >> 8;
    data[1] = color & 0xFF;

    LCD_WriteData(data, 2);
}


void LCD_FillScreen(uint16_t color)
{
    LCD_SetAddressWindow(
        0,
        0,
        LCD_WIDTH - 1,
        LCD_HEIGHT - 1
    );

    uint8_t data[2];

    data[0] = color >> 8;
    data[1] = color & 0xFF;

    /*
     * Send the same RGB565 pixel repeatedly.
     *
     * Sending in chunks avoids needing a 40 KB framebuffer.
     */
    uint8_t buffer[128];

    for (uint16_t i = 0; i < sizeof(buffer); i += 2)
    {
        buffer[i] = data[0];
        buffer[i + 1] = data[1];
    }

    LCD_CS_Low();
    LCD_DC_High();

    for (uint32_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; i += 64)
    {
        HAL_SPI_Transmit(
            &hspi2,
            buffer,
            sizeof(buffer),
            HAL_MAX_DELAY
        );
    }

    LCD_CS_High();
}


void LCD_DrawFastHLine(
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    if (x + width > LCD_WIDTH)
        width = LCD_WIDTH - x;

    LCD_SetAddressWindow(
        x,
        y,
        x + width - 1,
        y
    );

    uint8_t pixel[2];

    pixel[0] = color >> 8;
    pixel[1] = color & 0xFF;

    LCD_CS_Low();
    LCD_DC_High();

    for (uint16_t i = 0; i < width; i++)
    {
        HAL_SPI_Transmit(
            &hspi2,
            pixel,
            2,
            HAL_MAX_DELAY
        );
    }

    LCD_CS_High();
}


/* --------------------------------------------------------------------------
 * Text
 * -------------------------------------------------------------------------- */

void LCD_SetCursor(uint16_t x, uint16_t y)
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


/*
 * Very small 5x7 font.
 *
 * Add more characters here as needed.
 */
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


/*
 * Draw one character.
 *
 * Currently supports the characters needed for:
 *
 *     "Hello World!"
 *
 * Expand the font table later when needed.
 */
void LCD_WriteChar(char c)
{
    /*
     * Printable ASCII characters are 0x20 through 0x7E.
     */
    if (c < 0x20 || c > 0x7E)
        return;

    const uint8_t *glyph = font5x7[c - 0x20];

    for (uint8_t col = 0; col < 5; col++)
    {
        uint8_t bits = glyph[col];

        for (uint8_t row = 0; row < 7; row++)
        {
            if (bits & (1U << row))
            {
                for (uint8_t dx = 0; dx < text_size; dx++)
                {
                    for (uint8_t dy = 0; dy < text_size; dy++)
                    {
                        LCD_DrawPixel(
                            cursor_x + col * text_size + dx,
                            cursor_y + row * text_size + dy,
                            text_color
                        );
                    }
                }
            }
        }
    }

    /*
     * One blank column between characters.
     */
    cursor_x += 6 * text_size;
}


void LCD_Print(const char *str)
{
    while (*str)
    {
        if (*str == '\n')
        {
            cursor_x = 0;
            cursor_y += 8 * text_size;
        }
        else
        {
            LCD_WriteChar(*str);
        }

        str++;
    }
}