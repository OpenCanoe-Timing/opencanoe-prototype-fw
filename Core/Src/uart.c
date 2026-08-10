/**
 * @file uart.c
 * @author Alexander Ellul (igsalexcodes@gmail.com)
 * @brief UART Communication for the application.
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

#include "uart.h"
#include "stm32f4xx_hal.h"

#include <string.h>


extern UART_HandleTypeDef huart2;


#define UART_TX_QUEUE_SIZE   8
#define UART_TX_BUFFER_SIZE 128


/**
 * @brief UART transmit message structure.
 *
 * Stores a single message waiting to be transmitted.
 */
typedef struct
{
    uint8_t data[UART_TX_BUFFER_SIZE];
    uint16_t length;

} UART_Message_t;


static UART_Message_t tx_queue[UART_TX_QUEUE_SIZE];

static volatile uint8_t queue_head = 0;
static volatile uint8_t queue_tail = 0;

static volatile bool dma_busy = false;


/**
 * @brief Get the HAL UART handle for a UART port.
 *
 * @param uart UART port identifier.
 *
 * @return Pointer to UART handle.
 * @return NULL if the UART port is invalid.
 */
static UART_HandleTypeDef *UART_GetHandle(UART_Port_t uart)
{
    switch (uart)
    {
        case COMPUTER_UART:
            return &huart2;

        default:
            return NULL;
    }
}


/**
 * @brief Check if the transmit queue is empty.
 *
 * @return true Queue is empty.
 * @return false Queue contains messages.
 */
static bool UART_QueueEmpty(void)
{
    return queue_head == queue_tail;
}


/**
 * @brief Check if the transmit queue is full.
 *
 * @return true Queue is full.
 * @return false Queue has available space.
 */
static bool UART_QueueFull(void)
{
    return ((queue_head + 1) % UART_TX_QUEUE_SIZE) == queue_tail;
}


/**
 * @brief Start DMA transmission if possible.
 *
 * Starts sending the next queued message if DMA is idle.
 */
static void UART_Start_DMA(void)
{
    if (dma_busy || UART_QueueEmpty())
    {
        return;
    }


    dma_busy = true;


    HAL_UART_Transmit_DMA(
        &huart2,
        tx_queue[queue_tail].data,
        tx_queue[queue_tail].length
    );
}


/**
 * @brief Add data to the UART transmit queue.
 *
 * Data is copied into an internal buffer, allowing the caller
 * to reuse or modify the original buffer immediately.
 *
 * @param uart UART port identifier.
 * @param data Pointer to data buffer.
 * @param length Number of bytes to transmit.
 *
 * @return true Data queued successfully.
 * @return false Queue full or invalid parameters.
 */
bool UART_Write(UART_Port_t uart, const uint8_t *data, uint16_t length)
{
    if (UART_GetHandle(uart) == NULL)
    {
        return false;
    }


    if (data == NULL || length == 0)
    {
        return false;
    }


    if (length > UART_TX_BUFFER_SIZE)
    {
        return false;
    }


    if (UART_QueueFull())
    {
        return false;
    }


    memcpy(
        tx_queue[queue_head].data,
        data,
        length
    );

    tx_queue[queue_head].length = length;


    queue_head = (queue_head + 1) % UART_TX_QUEUE_SIZE;


    UART_Start_DMA();


    return true;
}


/**
 * @brief UART DMA transmit complete callback.
 *
 * Called by HAL when a DMA transmission finishes.
 *
 * @param huart UART handle that completed transmission.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == &huart2)
    {
        queue_tail = (queue_tail + 1) % UART_TX_QUEUE_SIZE;

        dma_busy = false;

        UART_Start_DMA();
    }
}