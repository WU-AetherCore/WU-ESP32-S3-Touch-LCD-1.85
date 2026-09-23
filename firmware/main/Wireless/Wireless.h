#pragma once

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "nvs_flash.h" 
#include "esp_netif.h"
#include "esp_event.h"

#include <stdio.h>
#include <string.h>
#include "esp_system.h"

extern uint16_t WIFI_NUM;
extern bool Scan_finish;
extern bool WiFi_Connected;
extern char WiFi_IP[16];

void Wireless_Init(void);
