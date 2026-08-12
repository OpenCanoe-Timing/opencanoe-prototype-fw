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

extern UART_HandleTypeDef huart1;
extern UART_HandleTypeDef huart2;

#define UART_TX_QUEUE_SIZE 8
#define UART_TX_BUFFER_SIZE 128

/**
 * @brief UART transmit message structure.
 *
 * Stores a single message waiting to be transmitted.
 */
typedef struct {
  uint8_t data[UART_TX_BUFFER_SIZE];
  uint16_t length;

} UART_Message_t;

/**
 * @brief Per-channel UART transmit state.
 *
 * Each UART port owns its own message queue and DMA
 * busy flag, so channels operate independently of one
 * another.
 */
typedef struct {
  UART_HandleTypeDef *handle;

  UART_Message_t tx_queue[UART_TX_QUEUE_SIZE];

  volatile uint8_t queue_head;
  volatile uint8_t queue_tail;

  volatile bool dma_busy;

} UART_Instance_t;

static UART_Instance_t uart_instances[UART_PORT_COUNT] = {
    [COMPUTER_UART] = {.handle = &huart2},
    [GNSS_UART] = {.handle = &huart1},
};

/**
 * @brief Get the transmit instance for a UART port.
 *
 * @param uart UART port identifier.
 *
 * @return Pointer to UART instance.
 * @return NULL if the UART port is invalid.
 */
static UART_Instance_t *UART_GetInstance(UART_Port_t uart) {
  if (uart >= UART_PORT_COUNT) {
    return NULL;
  }

  if (uart_instances[uart].handle == NULL) {
    return NULL;
  }

  return &uart_instances[uart];
}

/**
 * @brief Get the transmit instance matching a HAL UART handle.
 *
 * @param huart HAL UART handle.
 *
 * @return Pointer to UART instance.
 * @return NULL if no instance matches the handle.
 */
static UART_Instance_t *UART_GetInstanceByHandle(UART_HandleTypeDef *huart) {
  for (uint8_t i = 0; i < UART_PORT_COUNT; i++) {
    if (uart_instances[i].handle == huart) {
      return &uart_instances[i];
    }
  }

  return NULL;
}

/**
 * @brief Check if a transmit queue is empty.
 *
 * @param instance UART instance to check.
 *
 * @return true Queue is empty.
 * @return false Queue contains messages.
 */
static bool UART_QueueEmpty(UART_Instance_t *instance) {
  return instance->queue_head == instance->queue_tail;
}

/**
 * @brief Check if a transmit queue is full.
 *
 * @param instance UART instance to check.
 *
 * @return true Queue is full.
 * @return false Queue has available space.
 */
static bool UART_QueueFull(UART_Instance_t *instance) {
  return ((instance->queue_head + 1) % UART_TX_QUEUE_SIZE) ==
         instance->queue_tail;
}

/**
 * @brief Start DMA transmission if possible.
 *
 * Starts sending the next queued message on the given
 * instance if its DMA channel is idle.
 *
 * @param instance UART instance to service.
 */
static void UART_Start_DMA(UART_Instance_t *instance) {
  if (instance->dma_busy || UART_QueueEmpty(instance)) {
    return;
  }

  instance->dma_busy = true;

  HAL_UART_Transmit_DMA(instance->handle,
                        instance->tx_queue[instance->queue_tail].data,
                        instance->tx_queue[instance->queue_tail].length);
}

/**
 * @brief Add data to a UART transmit queue.
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
bool UART_Write(UART_Port_t uart, const uint8_t *data, uint16_t length) {
  UART_Instance_t *instance = UART_GetInstance(uart);

  if (instance == NULL) {
    return false;
  }

  if (data == NULL || length == 0) {
    return false;
  }

  if (length > UART_TX_BUFFER_SIZE) {
    return false;
  }

  if (UART_QueueFull(instance)) {
    return false;
  }

  memcpy(instance->tx_queue[instance->queue_head].data, data, length);

  instance->tx_queue[instance->queue_head].length = length;

  instance->queue_head = (instance->queue_head + 1) % UART_TX_QUEUE_SIZE;

  UART_Start_DMA(instance);

  return true;
}

/**
 * @brief UART DMA transmit complete callback.
 *
 * Called by HAL when a DMA transmission finishes. Advances
 * the queue belonging to whichever channel completed and
 * starts the next transmission on that same channel.
 *
 * @param huart UART handle that completed transmission.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
  UART_Instance_t *instance = UART_GetInstanceByHandle(huart);

  if (instance == NULL) {
    return;
  }

  instance->queue_tail = (instance->queue_tail + 1) % UART_TX_QUEUE_SIZE;

  instance->dma_busy = false;

  UART_Start_DMA(instance);
}