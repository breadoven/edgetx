/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "stm32_hal_ll.h"
#include "stm32_usart_driver.h"

#include "opentx.h"
#include "targets/horus/board.h"
#include "aux_serial_driver.h"

#include "fifo.h"
#include "dmafifo.h"

typedef DMAFifo<32> AuxSerialRxFifo;

struct AuxSerialState
{
  Fifo<uint8_t, 512>*  txFifo;
  AuxSerialRxFifo*     rxFifo;

  const stm32_usart_t* usart;
};

static void* aux_serial_init(struct AuxSerialState* st, const etx_serial_init* params)
{
  stm32_usart_init(st->usart, params);
  
  if (params->rx_enable && st->usart->rxDMA && !params->on_receive) {
    st->rxFifo->clear();
    stm32_usart_init_rx_dma(st->usart, st->rxFifo->buffer(), st->rxFifo->size());
  }

  return st;
}

static void aux_serial_putc(void* ctx, uint8_t c)
{
  AuxSerialState* st = (AuxSerialState*)ctx;
  if (st->txFifo->isFull()) return;
  st->txFifo->push(c);

  // Send is based on IRQ, so that this
  // only enables the corresponding IRQ
  stm32_usart_send_buffer(st->usart, &c, 1);
}

static void aux_serial_send_buffer(void* ctx, const uint8_t* data, uint8_t size)
{
  while(size > 0) {
    aux_serial_putc(ctx, *data++);
    size--;
  }
}

static void aux_wait_tx_completed(void* ctx)
{
  // TODO
  (void)ctx;
}

static int aux_get_byte(void* ctx, uint8_t* data)
{
  AuxSerialState* st = (AuxSerialState*)ctx;
  if (!st->rxFifo) return -1;
  return st->rxFifo->pop(*data);
}

void aux_serial_deinit(void* ctx)
{
  AuxSerialState* st = (AuxSerialState*)ctx;
  stm32_usart_deinit(st->usart);
}

#if defined(AUX_SERIAL)

Fifo<uint8_t, 512> auxSerialTxFifo;

#if defined(AUX_SERIAL_DMA_Stream_RX)
AuxSerialRxFifo auxSerialRxFifo __DMA (AUX_SERIAL_DMA_Stream_RX);
#else
AuxSerialRxFifo auxSerialRxFifo;
#endif

const LL_GPIO_InitTypeDef auxUSARTPinInit = {
  .Pin = AUX_SERIAL_GPIO_PIN_TX | AUX_SERIAL_GPIO_PIN_RX,
  .Mode = LL_GPIO_MODE_ALTERNATE,
  .Speed = LL_GPIO_SPEED_FREQ_LOW,
  .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
  .Pull = LL_GPIO_PULL_UP,
  .Alternate = AUX_SERIAL_GPIO_AF_LL,
};

const stm32_usart_t auxUSART = {
  .USARTx = AUX_SERIAL_USART,
  .GPIOx = AUX_SERIAL_GPIO,
  .pinInit = &auxUSARTPinInit,
  .IRQn = AUX_SERIAL_USART_IRQn,
  .IRQ_Prio = 7, // TODO: define constant
  .txDMA = nullptr,
  .txDMA_Stream = 0,
  .txDMA_Channel = 0,
#if defined(AUX_SERIAL_DMA_Stream_RX)
  .rxDMA = AUX_SERIAL_DMA_RX,
  .rxDMA_Stream = AUX_SERIAL_DMA_Stream_RX_LL,
  .rxDMA_Channel = AUX_SERIAL_DMA_Channel_RX,
#else
  .rxDMA = nullptr,
  .rxDMA_Stream = 0,
  .rxDMA_Channel = 0,
#endif
};

AuxSerialState auxSerialState = {
    .txFifo = &auxSerialTxFifo,
    .rxFifo = &auxSerialRxFifo,
    .usart = &auxUSART,
};

// TODO: grab context to get buffer
static uint8_t auxSerialOnSend(uint8_t* data)
{
  return auxSerialTxFifo.pop(*data);
}

// TODO: add to state
static etx_serial_callbacks_t auxSerialCb = {
  .on_send = auxSerialOnSend,
  .on_receive = nullptr,
  .on_error = nullptr,
};

static void* auxSerialInit(const etx_serial_init* params)
{
  return aux_serial_init(&auxSerialState, params);
}

const etx_serial_driver_t AuxSerialDriver = {
  .init = auxSerialInit,
  .deinit = aux_serial_deinit,
  .sendByte = aux_serial_putc,
  .sendBuffer = aux_serial_send_buffer,
  .waitForTxCompleted = aux_wait_tx_completed,
  .getByte = aux_get_byte,
  .getBaudrate = nullptr,
  .setReceiveCb = nullptr,
  .setBaudrateCb = nullptr,
};

extern "C" void AUX_SERIAL_USART_IRQHandler(void)
{
  DEBUG_INTERRUPT(INT_SER2);
  stm32_usart_isr(&auxUSART, &auxSerialCb);
}

#endif // AUX_SERIAL

#if defined(AUX2_SERIAL)

Fifo<uint8_t, 512> aux2SerialTxFifo;

AuxSerialRxFifo aux2SerialRxFifo __DMA (AUX2_SERIAL_DMA_Stream_RX);

const LL_GPIO_InitTypeDef aux2USARTPinInit = {
  .Pin = AUX2_SERIAL_GPIO_PIN_TX | AUX2_SERIAL_GPIO_PIN_RX,
  .Mode = LL_GPIO_MODE_ALTERNATE,
  .Speed = LL_GPIO_SPEED_FREQ_LOW,
  .OutputType = LL_GPIO_OUTPUT_PUSHPULL,
  .Pull = LL_GPIO_PULL_UP,
  .Alternate = AUX2_SERIAL_GPIO_AF_LL,
};

const stm32_usart_t aux2USART = {
  .USARTx = AUX2_SERIAL_USART,
  .GPIOx = AUX2_SERIAL_GPIO,
  .pinInit = &aux2USARTPinInit,
  .IRQn = AUX2_SERIAL_USART_IRQn,
  .IRQ_Prio = 7, // TODO: define constant
  .txDMA = nullptr,
  .txDMA_Stream = 0,
  .txDMA_Channel = 0,
  .rxDMA = AUX2_SERIAL_DMA_RX,
  .rxDMA_Stream = AUX2_SERIAL_DMA_Stream_RX_LL,
  .rxDMA_Channel = AUX2_SERIAL_DMA_Channel_RX,
};

AuxSerialState aux2SerialState = {
    .txFifo = &aux2SerialTxFifo,
    .rxFifo = &aux2SerialRxFifo,
    .usart = &aux2USART,
};

static uint8_t aux2SerialOnSend(uint8_t* data)
{
  return aux2SerialTxFifo.pop(*data);
}

static etx_serial_callbacks_t aux2SerialCb = {
  .on_send = aux2SerialOnSend,
  .on_receive = nullptr,
  .on_error = nullptr,
};

void* aux2SerialInit(const etx_serial_init* params)
{
  return aux_serial_init(&aux2SerialState, params);
}

extern "C" void AUX2_SERIAL_USART_IRQHandler(void)
{
  DEBUG_INTERRUPT(INT_SER2);
  stm32_usart_isr(&aux2USART, &aux2SerialCb);
}

const etx_serial_driver_t Aux2SerialDriver = {
  .init = aux2SerialInit,
  .deinit = aux_serial_deinit,
  .sendByte = aux_serial_putc,
  .sendBuffer = aux_serial_send_buffer,
  .waitForTxCompleted = aux_wait_tx_completed,
  .getByte = aux_get_byte,
  .getBaudrate = nullptr,
  .setReceiveCb = nullptr,
  .setBaudrateCb = nullptr,
};

#endif // AUX2_SERIAL
