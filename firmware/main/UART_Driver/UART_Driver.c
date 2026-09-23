#include "UART_Driver.h"

static const char *TAG_UART = "UART";

uart_ring_buf_t uart_rx_ring = {0};
uart_ring_buf_t uart_tx_ring = {0};

static QueueHandle_t uart_queue = NULL;

static void uart_event_task(void *arg)
{
    uart_event_t event;
    uint8_t data[128];
    
    while (1) {
        if (xQueueReceive(uart_queue, &event, pdMS_TO_TICKS(100))) {
            switch (event.type) {
                case UART_DATA:
                    if (event.size > 0) {
                        int len = uart_read_bytes(UART_PORT_NUM, data, event.size, pdMS_TO_TICKS(20));
                        if (len > 0) {
                            // Store in RX ring buffer
                            for (int i = 0; i < len; i++) {
                                uint16_t next = (uart_rx_ring.head + 1) % RX_RING_SIZE;
                                if (next != uart_rx_ring.tail) {
                                    uart_rx_ring.data[uart_rx_ring.head] = data[i];
                                    uart_rx_ring.head = next;
                                    if (uart_rx_ring.count < RX_RING_SIZE) {
                                        uart_rx_ring.count++;
                                    } else {
                                        uart_rx_ring.tail = (uart_rx_ring.tail + 1) % RX_RING_SIZE;
                                    }
                                }
                            }
                        }
                    }
                    break;
                case UART_FIFO_OVF:
                    ESP_LOGW(TAG_UART, "UART FIFO Overflow");
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(uart_queue);
                    break;
                case UART_BUFFER_FULL:
                    ESP_LOGW(TAG_UART, "UART Ring Buffer Full");
                    uart_flush_input(UART_PORT_NUM);
                    xQueueReset(uart_queue);
                    break;
                default:
                    break;
            }
        }
    }
    vTaskDelete(NULL);
}

void UART_Driver_Init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    // Create UART event task
    xTaskCreatePinnedToCore(uart_event_task, "uart_event_task", 4096, NULL, 5, NULL, 1);
    
    ESP_LOGI(TAG_UART, "UART initialized: TX=%d, RX=%d, Baud=%d", UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
}

esp_err_t UART_Send_Data(const uint8_t *data, size_t len)
{
    if (len == 0) return ESP_OK;
    
    // Store in TX ring buffer
    for (size_t i = 0; i < len; i++) {
        uint16_t next = (uart_tx_ring.head + 1) % RX_RING_SIZE;
        if (next != uart_tx_ring.tail) {
            uart_tx_ring.data[uart_tx_ring.head] = data[i];
            uart_tx_ring.head = next;
            if (uart_tx_ring.count < RX_RING_SIZE) {
                uart_tx_ring.count++;
            } else {
                uart_tx_ring.tail = (uart_tx_ring.tail + 1) % RX_RING_SIZE;
            }
        }
    }
    
    int written = uart_write_bytes(UART_PORT_NUM, data, len);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t UART_Send_String(const char *str)
{
    return UART_Send_Data((const uint8_t *)str, strlen(str));
}

uint16_t UART_Get_RX_Data(char *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (uart_rx_ring.tail != uart_rx_ring.head && count < max_len - 1) {
        buf[count++] = uart_rx_ring.data[uart_rx_ring.tail];
        uart_rx_ring.tail = (uart_rx_ring.tail + 1) % RX_RING_SIZE;
        if (uart_rx_ring.count > 0) uart_rx_ring.count--;
    }
    buf[count] = '\0';
    return count;
}

uint16_t UART_Get_TX_Data(char *buf, uint16_t max_len)
{
    uint16_t count = 0;
    while (uart_tx_ring.tail != uart_tx_ring.head && count < max_len - 1) {
        buf[count++] = uart_tx_ring.data[uart_tx_ring.tail];
        uart_tx_ring.tail = (uart_tx_ring.tail + 1) % RX_RING_SIZE;
        if (uart_tx_ring.count > 0) uart_tx_ring.count--;
    }
    buf[count] = '\0';
    return count;
}

void UART_Clear_RX_Buffer(void)
{
    uart_rx_ring.head = 0;
    uart_rx_ring.tail = 0;
    uart_rx_ring.count = 0;
    memset(uart_rx_ring.data, 0, RX_RING_SIZE);
}

void UART_Clear_TX_Buffer(void)
{
    uart_tx_ring.head = 0;
    uart_tx_ring.tail = 0;
    uart_tx_ring.count = 0;
    memset(uart_tx_ring.data, 0, RX_RING_SIZE);
}
