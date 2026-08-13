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

#define UART_RX_BUFFER_SIZE 256

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
 * @brief Per-channel UART transmit and receive state.
 *
 * Each UART port owns its own message queue, DMA busy flag,
 * and receive buffer, so channels operate independently of
 * one another.
 */
typedef struct {
  UART_HandleTypeDef *handle;

  UART_Message_t tx_queue[UART_TX_QUEUE_SIZE];

  volatile uint8_t queue_head;
  volatile uint8_t queue_tail;

  volatile bool dma_busy;

  bool rx_enabled;

  uint8_t rx_buffer[UART_RX_BUFFER_SIZE];
  volatile uint16_t rx_old_pos;

  UART_RxCallback_t rx_callback;

} UART_Instance_t;

static UART_Instance_t uart_instances[UART_PORT_COUNT] = {
    /* USART2 only has TX wired to DMA on this board (DMA1 Stream6),
     * so its RX side stays interrupt/polled and is never started here. */
    [COMPUTER_UART] = {.handle = &huart2, .rx_enabled = false},

    /* USART1 only has RX wired to DMA on this board (DMA2 Stream2),
     * feeding the idle-line reception used to frame NMEA sentences. */
    [GNSS_UART] = {.handle = &huart1, .rx_enabled = true},
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

/**
 * @brief Start circular DMA reception with idle-line detection.
 *
 * The receive buffer is treated as a ring: DMA keeps filling
 * it in the background and HAL_UARTEx_RxEventCallback fires
 * whenever the line goes idle (e.g. between NMEA sentences)
 * or the buffer wraps, whichever happens first.
 *
 * Does nothing if the channel has no RX DMA stream linked
 * (i.e. CubeMX was not configured to give this UART an RX
 * DMA request), since starting reception without one would
 * fault inside the HAL.
 *
 * @param instance UART instance to start receiving on.
 */
static void UART_Start_Rx(UART_Instance_t *instance) {
  if (instance->handle->hdmarx == NULL) {
    return;
  }

  instance->rx_old_pos = 0;

  HAL_UARTEx_ReceiveToIdle_DMA(instance->handle, instance->rx_buffer,
                               UART_RX_BUFFER_SIZE);

  /* Half-transfer events are not useful here and would split
   * frames arbitrarily, so they are suppressed. Only idle-line
   * and full-buffer wrap events reach the event callback. */
  __HAL_DMA_DISABLE_IT(instance->handle->hdmarx, DMA_IT_HT);
}

/**
 * @brief Deliver newly received bytes to the registered callback.
 *
 * Handles the case where the new write position has wrapped
 * around the end of the ring buffer by splitting the delivery
 * into two calls.
 *
 * @param instance UART instance the data was received on.
 * @param port UART port identifier, passed through to the callback.
 * @param new_pos Ring buffer position DMA has written up to.
 */
static void UART_Deliver_Rx(UART_Instance_t *instance, UART_Port_t port,
                            uint16_t new_pos) {
  if (instance->rx_callback == NULL) {
    instance->rx_old_pos = new_pos;

    return;
  }

  if (new_pos == instance->rx_old_pos) {
    return;
  }

  if (new_pos > instance->rx_old_pos) {
    instance->rx_callback(port, &instance->rx_buffer[instance->rx_old_pos],
                          new_pos - instance->rx_old_pos);
  } else {
    instance->rx_callback(port, &instance->rx_buffer[instance->rx_old_pos],
                          UART_RX_BUFFER_SIZE - instance->rx_old_pos);

    if (new_pos > 0) {
      instance->rx_callback(port, instance->rx_buffer, new_pos);
    }
  }

  instance->rx_old_pos = new_pos;
}

/**
 * @brief Initialise UART reception for all channels.
 *
 * Must be called once after the HAL UART and DMA peripherals
 * have been initialised (i.e. after MX_USARTx_UART_Init and
 * MX_DMA_Init), and before any received data is expected.
 */
void UART_Init(void) {
  for (uint8_t i = 0; i < UART_PORT_COUNT; i++) {
    if (uart_instances[i].handle != NULL && uart_instances[i].rx_enabled) {
      UART_Start_Rx(&uart_instances[i]);
    }
  }
}

/**
 * @brief Register a callback to receive incoming data for a UART port.
 *
 * Only one callback is supported per port; registering again
 * replaces the previous callback.
 *
 * @param uart UART port identifier.
 * @param callback Function to call with received data.
 *
 * @return true Callback registered successfully.
 * @return false UART port is invalid.
 */
bool UART_RegisterRxCallback(UART_Port_t uart, UART_RxCallback_t callback) {
  UART_Instance_t *instance = UART_GetInstance(uart);

  if (instance == NULL) {
    return false;
  }

  instance->rx_callback = callback;

  return true;
}

/**
 * @brief UART reception event callback.
 *
 * Called by HAL on idle-line detection or when the DMA
 * transfer completes (buffer wrap). Forwards the newly
 * received bytes to the owning channel's registered callback.
 *
 * @param huart UART handle that generated the event.
 * @param Size Ring buffer position DMA has written up to.
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
  for (uint8_t i = 0; i < UART_PORT_COUNT; i++) {
    if (uart_instances[i].handle == huart) {
      UART_Deliver_Rx(&uart_instances[i], (UART_Port_t)i, Size);

      return;
    }
  }
}

/**
 * @brief UART error callback.
 *
 * Called by HAL on framing, noise, or overrun errors. Circular
 * DMA reception does not recover from an error on its own, so
 * it is restarted here to keep the channel alive.
 *
 * @param huart UART handle that reported the error.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  UART_Instance_t *instance = UART_GetInstanceByHandle(huart);

  if (instance == NULL || !instance->rx_enabled) {
    return;
  }

  UART_Start_Rx(instance);
}