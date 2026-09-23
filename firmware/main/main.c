#include "ST77916.h"
#include "PCF85063.h"
#include "QMI8658.h"
#include "SD_MMC.h"
#include "TCA9554PWR.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"
#include "Hardware_Test.h"
#include "LVGL_Music.h"
#include "PCM5101.h"
#include "Touch_Calibration.h"

static void gui_task(void *arg) {
    Touch_Calibration_Init();
    Music_Player_Init();
    Test_UI_Init();
    ESP_LOGI("MAIN", "Device Lab ready. UART1 TX=43 RX=44; console=USB");
    for(;;) {Test_UI_Debug_Poll();lv_timer_handler();vTaskDelay(1);}
}

void app_main(void) {
    ESP_LOGI("MAIN", "Device Lab: ESP32-S3 Touch LCD 1.85");
    PWR_Init(); BAT_Init(); I2C_Init(); TCA9554PWR_Init(0x18); /* P3/P4 are IMU interrupt inputs. */
    Flash_Searching();
    uint8_t probe=0;
    if(I2C_Read(PCF85063_ADDRESS,RTC_CTRL_1_ADDR,&probe,1)==ESP_OK) PCF85063_Init();
    if(I2C_Read(QMI8658_L_SLAVE_ADDRESS,QMI8658_WHO_AM_I,&probe,1)==ESP_OK) QMI8658_Init();
    SD_Init(); LCD_Init(); Audio_Init();
    Set_EXIO(TCA9554_EXIO6,true);Set_EXIO(TCA9554_EXIO7,true);Set_EXIO(TCA9554_EXIO8,true);
    LVGL_Init();Test_Service_Init();
    BaseType_t ok=xTaskCreatePinnedToCore(gui_task,"device_ui",12288,NULL,2,NULL,1);
    ESP_ERROR_CHECK(ok==pdPASS?ESP_OK:ESP_ERR_NO_MEM);
}
