#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define TEST_AP_MAX 16
 typedef struct {
    float accel[3], gyro[3], battery, mic_db;
    int16_t wave[64];
    uint8_t hour, minute, second;
    bool mic_ok, speaker_ok, imu_ok, rtc_ok;
    bool wifi_busy, connected, associated;
    char network[96];
    char ssids[TEST_AP_MAX][33];
    int8_t rssi[TEST_AP_MAX];
    uint16_t aps;
    uint32_t scan_version;
    int key;
} test_state_t;
void Test_Service_Init(void);
void Test_State(test_state_t *out);
esp_err_t Test_Wifi_Scan(void);
esp_err_t Test_Wifi_Connect(const char *ssid, const char *password);
void Test_Tone(void);
void Test_UI_Init(void);

void Test_UI_Debug_Poll(void);

void Test_UI_ShowHome(void);
