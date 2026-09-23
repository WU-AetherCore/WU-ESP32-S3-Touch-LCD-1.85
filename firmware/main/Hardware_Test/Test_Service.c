#include "Hardware_Test.h"
#include "PCM5101.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "QMI8658.h"
#include "PCF85063.h"
#include "BAT_Driver.h"
#include "PWR_Key.h"
#include "driver/gpio.h"

static test_state_t state;
static portMUX_TYPE guard = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t requests;
static i2s_chan_handle_t mic, speaker;
static bool wifi_ready;

typedef struct { bool scan; char ssid[33], pass[65]; } request_t;
void Test_State(test_state_t *out) { portENTER_CRITICAL(&guard); *out=state; portEXIT_CRITICAL(&guard); }
static void net_status(const char *s, bool busy) {
    portENTER_CRITICAL(&guard);
    snprintf(state.network,sizeof(state.network),"%s",s); state.wifi_busy=busy;
    portEXIT_CRITICAL(&guard);
}
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if(base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e=data;
        char text[96]; snprintf(text,sizeof(text),"已连接  " IPSTR,IP2STR(&e->ip_info.ip));
        portENTER_CRITICAL(&guard); state.connected=true; state.associated=true; portEXIT_CRITICAL(&guard);
        net_status(text,false);
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_CONNECTED) {
        portENTER_CRITICAL(&guard);state.associated=true;portEXIT_CRITICAL(&guard);
        net_status("已关联，等待地址",true);
    } else if(base==IP_EVENT && id==IP_EVENT_STA_LOST_IP) {
        portENTER_CRITICAL(&guard);state.connected=false;portEXIT_CRITICAL(&guard);
        net_status("地址丢失，请重连",false);
    } else if(base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e=data;
        portENTER_CRITICAL(&guard); state.connected=false;state.associated=false; portEXIT_CRITICAL(&guard);
        char text[96]; snprintf(text,sizeof(text),"已断开（原因 %u）",e->reason);
        net_status(text,false);
    }
}
static void wifi_worker(void *arg) {
    request_t req;
    int64_t deadline=0;
    for(;;) {
        if(!xQueueReceive(requests,&req,pdMS_TO_TICKS(500))) {
            if(deadline && esp_timer_get_time()>deadline) {
                test_state_t copy; Test_State(&copy); deadline=0;
                if(!copy.connected && copy.wifi_busy) {net_status(copy.associated?"获取地址超时，请重试":"连接超时，请重试",false);}
            }
            continue;
        }
        if(req.scan) {
            net_status("正在扫描 2.4 GHz...",true);
            wifi_scan_config_t cfg={0};
            esp_err_t err=esp_wifi_scan_start(&cfg,true);
            wifi_ap_record_t ap[TEST_AP_MAX]; uint16_t n=TEST_AP_MAX;
            if(err==ESP_OK) err=esp_wifi_scan_get_ap_records(&n,ap);
            if(err==ESP_OK) {
                portENTER_CRITICAL(&guard);
                state.aps=n;
                for(int i=0;i<n;i++) { memcpy(state.ssids[i],ap[i].ssid,32); state.ssids[i][32]=0; state.rssi[i]=ap[i].rssi; }
                state.scan_version++;
                portEXIT_CRITICAL(&guard);
                net_status(n?"选择网络或输入名称":"未发现网络",false);
            } else net_status(esp_err_to_name(err),false);
        } else {
            /* Stop/start serializes a switch with disconnect events before applying credentials. */
            esp_wifi_stop();
            wifi_config_t cfg={0};
            memcpy(cfg.sta.ssid,req.ssid,strlen(req.ssid));
            memcpy(cfg.sta.password,req.pass,strlen(req.pass));
            cfg.sta.pmf_cfg.capable=true;
            portENTER_CRITICAL(&guard);state.connected=false;portEXIT_CRITICAL(&guard);
            esp_err_t err=esp_wifi_set_config(WIFI_IF_STA,&cfg);
            if(err==ESP_OK) err=esp_wifi_start();
            if(err==ESP_OK) err=esp_wifi_connect();
            memset(&cfg,0,sizeof(cfg)); memset(&req,0,sizeof(req));
            net_status(err==ESP_OK?"正在连接...":esp_err_to_name(err),err==ESP_OK);
            deadline=err==ESP_OK?esp_timer_get_time()+30000000:0;
        }
    }
}
static esp_err_t submit(request_t *r) {
    if(!wifi_ready) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&guard);
    bool busy=state.wifi_busy;
    if(!busy) state.wifi_busy=true;
    portEXIT_CRITICAL(&guard);
    if(busy) return ESP_ERR_INVALID_STATE;
    if(xQueueSend(requests,r,0)!=pdTRUE) {net_status("无线网络忙",false); return ESP_FAIL;}
    return ESP_OK;
}
esp_err_t Test_Wifi_Scan(void) { request_t r={.scan=true}; return submit(&r); }
esp_err_t Test_Wifi_Connect(const char *ssid,const char *password) {
    size_t s=strlen(ssid),p=strlen(password);
    if(!s || s>32 || p>63 || (p && p<8)) return ESP_ERR_INVALID_ARG;
    request_t r={0}; memcpy(r.ssid,ssid,s); memcpy(r.pass,password,p);
    esp_err_t err=submit(&r); memset(&r,0,sizeof(r)); return err;
}
static void mic_worker(void *arg) {
    int32_t samples[256];
    for(;;) {
        size_t bytes=0;
        esp_err_t err=i2s_channel_read(mic,samples,sizeof(samples),&bytes,250);
        int count=bytes/sizeof(int32_t); float mean=0,energy=0;
        if(err!=ESP_OK || count<64) {
            portENTER_CRITICAL(&guard); state.mic_ok=false; portEXIT_CRITICAL(&guard);
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        for(int i=0;i<count;i++) mean+=(float)(samples[i]>>8)/8388608.0f;
        mean/=count;
        int16_t wave[64];
        for(int i=0;i<count;i++) {float v=(float)(samples[i]>>8)/8388608.0f-mean; energy+=v*v;}
        for(int i=0;i<64;i++) {float v=((float)(samples[i*count/64]>>8)/8388608.0f-mean)*8000; wave[i]=(int16_t)fmaxf(-100,fminf(100,v));}
        float db=20*log10f(fmaxf(sqrtf(energy/count),0.000001f));
        portENTER_CRITICAL(&guard); state.mic_db=db; state.mic_ok=true; memcpy(state.wave,wave,sizeof(wave)); portEXIT_CRITICAL(&guard);
    }
}
void Test_Tone(void) { Audio_Test_Tone(); }
static void sensor_worker(void *arg) {
    for(;;) {
        uint8_t raw[12]={0};
        bool imu_ok=I2C_Read(QMI8658_L_SLAVE_ADDRESS,QMI8658_AX_L,raw,sizeof(raw))==ESP_OK;
        /* Existing board setup selects +/-4g and +/-64 deg/s. One burst = coherent sample. */
        float a[3],g[3];
        for(int i=0;i<3;i++) {a[i]=(int16_t)(raw[i*2]|raw[i*2+1]<<8)*(4.0f/32768);g[i]=(int16_t)(raw[6+i*2]|raw[7+i*2]<<8)*(64.0f/32768);}
        uint8_t probe=0;
        bool rtc_ok=I2C_Read(PCF85063_ADDRESS,RTC_CTRL_1_ADDR,&probe,1)==ESP_OK;
        if(rtc_ok) { PCF85063_Loop(); rtc_ok=datetime.hour<24 && datetime.minute<60 && datetime.second<60; }
        BAT_Get_Volts(); PWR_Loop();
        portENTER_CRITICAL(&guard);
        memcpy(state.accel,a,sizeof(a));memcpy(state.gyro,g,sizeof(g));
        state.imu_ok=imu_ok;state.rtc_ok=rtc_ok;
        state.battery=BAT_analogVolts;state.hour=datetime.hour;state.minute=datetime.minute;state.second=datetime.second;
        state.key=gpio_get_level(PWR_KEY_Input_PIN);
        portEXIT_CRITICAL(&guard);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
void Test_Service_Init(void) {
    state.mic_db=-120;
    xTaskCreate(sensor_worker,"test_sensors",4096,NULL,2,NULL);
    i2s_chan_config_t ch=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    esp_err_t err=i2s_new_channel(&ch,NULL,&mic);
    i2s_std_config_t cfg={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_MONO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=15,.ws=2,.dout=I2S_GPIO_UNUSED,.din=39}
    };
    cfg.slot_cfg.slot_mask=I2S_STD_SLOT_RIGHT;
    if(err==ESP_OK) err=i2s_channel_init_std_mode(mic,&cfg);
    if(err==ESP_OK) err=i2s_channel_enable(mic);
    if(err==ESP_OK) xTaskCreate(mic_worker,"test_mic",4096,NULL,3,NULL);
    else ESP_LOGE("TEST","Microphone: %s",esp_err_to_name(err));
    /* I2S0 is reserved by PCM5101 music playback. Do not create a second
     * channel here; doing so steals the controller and makes music silent. */
    speaker = NULL;
    state.speaker_ok = Audio_Ready();
    err=nvs_flash_init();
    /* Do not erase user NVS on initialization errors. */
    if(err==ESP_OK) err=esp_netif_init();
    if(err==ESP_OK) err=esp_event_loop_create_default();
    if(err==ESP_OK && !esp_netif_create_default_wifi_sta()) err=ESP_ERR_NO_MEM;
    wifi_init_config_t wc=WIFI_INIT_CONFIG_DEFAULT();
    if(err==ESP_OK) err=esp_wifi_init(&wc);
    if(err==ESP_OK) err=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(err==ESP_OK) err=esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL);
    if(err==ESP_OK) err=esp_event_handler_register(IP_EVENT,ESP_EVENT_ANY_ID,wifi_event,NULL);
    if(err==ESP_OK) err=esp_wifi_set_mode(WIFI_MODE_STA);
    if(err==ESP_OK) err=esp_wifi_start();
    requests=xQueueCreate(1,sizeof(request_t));
    if(err==ESP_OK && requests && xTaskCreate(wifi_worker,"test_wifi",6144,NULL,2,NULL)==pdPASS) {
        wifi_ready=true;net_status("点击扫描查找网络",false);
    } else net_status("无线网络初始化失败",false);
}
