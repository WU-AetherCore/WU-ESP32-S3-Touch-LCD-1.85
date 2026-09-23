#pragma once

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

// UART Configuration
#define UART_PORT_NUM       UART_NUM_1
#define UART_TX_PIN         43
#define UART_RX_PIN         44
#define UART_BAUD_RATE      115200
#define UART_BUF_SIZE       1024
#define UART_RX_LINE_BUF    512

// Ring buffer for received data display
#define RX_RING_SIZE        4096

typedef struct {
    char data[RX_RING_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
    volatile uint16_t count;
} uart_ring_buf_t;

extern uart_ring_buf_t uart_rx_ring;
extern uart_ring_buf_t uart_tx_ring;

void UART_Driver_Init(void);
esp_err_t UART_Send_Data(const uint8_t *data, size_t len);
esp_err_t UART_Send_String(const char *str);
uint16_t UART_Get_RX_Data(char *buf, uint16_t max_len);
uint16_t UART_Get_TX_Data(char *buf, uint16_t max_len);
void UART_Clear_RX_Buffer(void);
void UART_Clear_TX_Buffer(void);
