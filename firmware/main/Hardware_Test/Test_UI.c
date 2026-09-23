#include "Hardware_Test.h"
#include "lvgl.h"
#include "ST77916.h"
#include "SD_MMC.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "LVGL_Music.h"
#include "PCM5101.h"
#include "PCF85063.h"
#include "Touch_Calibration.h"
#include "LVGL_Media.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define BG 0x0B101A
#define CARD 0x172233
#define ACCENT 0x47DECB
#define MUTED 0x8A9BB0
static lv_obj_t *lab_screen,*body,*title,*footer,*back;
static lv_obj_t *values,*bar,*chart,*ssid,*password,*net,*aps,*logview,*sendtext,*baud,*hexbox,*crlf;
static lv_obj_t *overlay,*editor,*keyboard,*edit_target;
static lv_chart_series_t *series;
static test_state_t snapshot;
static int page;
static uint32_t ap_version=UINT32_MAX,flash_bytes,partition_bytes;
static uint64_t received,sent,dropped;
static unsigned uart_errors;
static portMUX_TYPE uart_guard=portMUX_INITIALIZER_UNLOCKED;
typedef struct {int n;uint8_t bytes[96];} rx_packet_t;
static QueueHandle_t rx_packets,uart_events;
static void uart_worker(void *arg) {
    for(;;) {
        rx_packet_t packet;
        packet.n=uart_read_bytes(UART_NUM_1,packet.bytes,sizeof(packet.bytes),pdMS_TO_TICKS(10));
        if(packet.n>0) {
            bool lost=xQueueSend(rx_packets,&packet,0)!=pdTRUE;
            portENTER_CRITICAL(&uart_guard);received+=packet.n;if(lost)dropped+=packet.n;portEXIT_CRITICAL(&uart_guard);
        }
        uart_event_t event;
        while(xQueueReceive(uart_events,&event,0)) {
            if(event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL || event.type==UART_FRAME_ERR || event.type==UART_PARITY_ERR) {
                portENTER_CRITICAL(&uart_guard);uart_errors++;portEXIT_CRITICAL(&uart_guard);
            }
        }
    }
}
static bool uart_ok;
static char uart_log[4096];
static uint32_t colors[]={0xFFFFFF,0xFF3048,0x28D7A1,0x448BFF,0x000000};
extern const lv_font_t font_music_cjk;
static lv_obj_t *color_screen;
static bool key1_last=true,key2_last=true,key3_last=true;
static const gpio_num_t KEY_MUSIC=GPIO_NUM_0, KEY_PREV=GPIO_NUM_12, KEY_NEXT=GPIO_NUM_13;
static unsigned color_index;
static void show_page(int id);
static void music_back(void) { Test_UI_ShowHome(); }
static void calibrate_touch(lv_event_t *e) {Touch_Calibration_Start();}

/* Use libc: this project's LVGL minimal printf has float formatting disabled. */
static void label_fmt(lv_obj_t *obj,const char *format,...) {
    char text[640];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);lv_label_set_text(obj,text);
}

static lv_obj_t *label(lv_obj_t *p,const char *text) {
    lv_obj_t *o=lv_label_create(p);lv_label_set_text(o,text);
    lv_obj_set_width(o,LV_PCT(100));lv_obj_set_style_text_font(o,&font_music_cjk,0);lv_label_set_long_mode(o,LV_LABEL_LONG_WRAP);
    return o;
}
static void hint(lv_obj_t *p,const char *text) {lv_obj_t *o=label(p,text);lv_obj_set_style_text_color(o,lv_color_hex(MUTED),0);lv_obj_set_style_text_font(o,&font_music_cjk,0);}
static lv_obj_t *button(lv_obj_t *p,const char *text,lv_event_cb_t cb,void *data) {
    lv_obj_t *o=lv_btn_create(p);lv_obj_set_size(o,LV_PCT(100),40);
    lv_obj_set_style_bg_color(o,lv_color_hex(CARD),0);lv_obj_set_style_radius(o,12,0);
    lv_obj_set_style_shadow_width(o,0,0);
    lv_obj_t *l=lv_label_create(o);lv_label_set_text(l,text);lv_obj_set_style_text_font(l,&font_music_cjk,0);lv_obj_center(l);
    lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);return o;
}
static void navigate(lv_event_t *e) {show_page((int)(intptr_t)lv_event_get_user_data(e));}
static void close_editor(lv_event_t *e) {
    if(lv_event_get_code(e)==LV_EVENT_READY) lv_textarea_set_text(edit_target,lv_textarea_get_text(editor));
    lv_textarea_set_text(editor,"");
    lv_obj_del(overlay);overlay=NULL;editor=NULL;keyboard=NULL;edit_target=NULL;
}
static void open_editor(lv_event_t *e) {
    if(overlay) return;
    edit_target=lv_event_get_target(e);
    overlay=lv_obj_create(lv_layer_top());lv_obj_set_size(overlay,360,360);lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay,lv_color_hex(BG),0);lv_obj_set_style_border_width(overlay,0,0);
    lv_obj_set_style_pad_all(overlay,0,0);lv_obj_clear_flag(overlay,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *caption=lv_label_create(overlay);lv_label_set_text(caption,"输入 / 确认应用");lv_obj_set_style_text_font(caption,&font_music_cjk,0);lv_obj_align(caption,LV_ALIGN_TOP_MID,0,36);
    editor=lv_textarea_create(overlay);lv_obj_set_size(editor,248,42);lv_obj_set_pos(editor,56,78);
    lv_obj_set_style_bg_color(editor,lv_color_hex(CARD),0);lv_obj_set_style_border_width(editor,0,0);
    lv_textarea_set_one_line(editor,true);lv_textarea_set_max_length(editor,lv_textarea_get_max_length(edit_target));
    lv_textarea_set_password_mode(editor,edit_target==password);lv_textarea_set_text(editor,lv_textarea_get_text(edit_target));
    keyboard=lv_keyboard_create(overlay);lv_obj_set_align(keyboard,LV_ALIGN_TOP_LEFT);lv_obj_set_size(keyboard,264,144);lv_obj_set_pos(keyboard,48,128);
    lv_obj_set_style_text_font(keyboard,&lv_font_montserrat_14,LV_PART_ITEMS);
    lv_keyboard_set_textarea(keyboard,editor);
    lv_obj_add_event_cb(keyboard,close_editor,LV_EVENT_READY,NULL);lv_obj_add_event_cb(keyboard,close_editor,LV_EVENT_CANCEL,NULL);
}
static lv_obj_t *input(lv_obj_t *p,const char *placeholder,int max,bool secret) {
    lv_obj_t *o=lv_textarea_create(p);lv_obj_set_size(o,LV_PCT(100),42);lv_textarea_set_one_line(o,true);
    lv_textarea_set_placeholder_text(o,placeholder);lv_textarea_set_max_length(o,max);lv_textarea_set_password_mode(o,secret);
    lv_obj_set_style_bg_color(o,lv_color_hex(CARD),0);lv_obj_set_style_border_width(o,0,0);
    lv_obj_add_event_cb(o,open_editor,LV_EVENT_CLICKED,NULL);return o;
}
static void scan(lv_event_t *e) {if(Test_Wifi_Scan()!=ESP_OK) lv_label_set_text(net,"Wi-Fi 忙或不可用");}
static void connect_wifi(lv_event_t *e) {
    esp_err_t err=Test_Wifi_Connect(lv_textarea_get_text(ssid),lv_textarea_get_text(password));
    if(err==ESP_ERR_INVALID_ARG) lv_label_set_text(net,"SSID：1-32 字节\n密码：开放网络留空，或填写 8-63 字节");
    else if(err!=ESP_OK) lv_label_set_text(net,"Wi-Fi 忙或不可用");
    else lv_textarea_set_text(password,"");
}
static void position_dropdown(lv_obj_t *dropdown) {
    if(!lv_dropdown_is_open(dropdown)) return;
    lv_obj_t *list=lv_dropdown_get_list(dropdown);
    lv_obj_set_width(list,244);
    lv_obj_set_style_max_height(list,112,0);
    lv_obj_update_layout(list);
    lv_obj_set_pos(list,58,118);
}
static void dropdown_clicked(lv_event_t *e) { position_dropdown(lv_event_get_target(e)); }
static void select_ap(lv_event_t *e) {
    unsigned i=lv_dropdown_get_selected(aps);
    if(i<snapshot.aps) lv_textarea_set_text(ssid,snapshot.ssids[i]);
}
static void append_log(const char *direction,const uint8_t *bytes,int n) {
    char line[512];int offset=snprintf(line,sizeof(line),"%s ",direction);
    bool hex=hexbox && lv_obj_has_state(hexbox,LV_STATE_CHECKED);
    for(int i=0;i<n && offset<(int)sizeof(line)-6;i++) {
        unsigned c=bytes[i];
        if(hex) offset+=snprintf(line+offset,sizeof(line)-offset,"%02X ",c);
        else if(c=='\n' || c=='\r' || (c>=32 && c<127)) line[offset++]=(char)c;
        else offset+=snprintf(line+offset,sizeof(line)-offset,"\\x%02X",c);
    }
    line[offset++]='\n';line[offset]=0;
    size_t old=strlen(uart_log);
    if(old+offset>=sizeof(uart_log)) {size_t drop=old+offset-sizeof(uart_log)+1; memmove(uart_log,uart_log+drop,old-drop+1);}
    strcat(uart_log,line);
    
}
static void send_uart(lv_event_t *e) {
    if(!uart_ok) return;
    const char *s=lv_textarea_get_text(sendtext);uint8_t bytes[100];int n=0;
    if(lv_obj_has_state(hexbox,LV_STATE_CHECKED)) {
        int high=-1;
        for(;*s;s++) {
            if(isspace((unsigned char)*s)) continue;
            int c=tolower((unsigned char)*s),v=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
            if(v<0) {lv_label_set_text(net,"请输入字节对，如 01 A5 FF");return;}
            if(high<0) high=v;else {bytes[n++]=(high<<4)|v;high=-1;}
        }
        if(high>=0) {lv_label_set_text(net,"十六进制需要完整字节对");return;}
    } else {n=strlen(s);memcpy(bytes,s,n);}
    if(lv_obj_has_state(crlf,LV_STATE_CHECKED)) {bytes[n++]='\r';bytes[n++]='\n';}
    int written=uart_write_bytes(UART_NUM_1,bytes,n);
    if(written>0) {sent+=written;append_log("TX",bytes,written);}
}
static void clear_uart(lv_event_t *e) {uart_log[0]=0;portENTER_CRITICAL(&uart_guard);sent=received=dropped=0;uart_errors=0;portEXIT_CRITICAL(&uart_guard);lv_textarea_set_text(logview,"");}
static void change_baud(lv_event_t *e) {static const int rates[]={9600,19200,38400,57600,115200,230400,460800,921600};if(uart_ok) uart_set_baudrate(UART_NUM_1,rates[lv_dropdown_get_selected(baud)]);}
static void tone(lv_event_t *e) {Test_Tone();}
static void brightness(lv_event_t *e) {Set_Backlight(lv_slider_get_value(lv_event_get_target(e)));}
static void next_color(lv_event_t *e) {
    if(++color_index>=sizeof(colors)/sizeof(colors[0])) {lv_obj_del(color_screen);color_screen=NULL;return;}
    lv_obj_set_style_bg_color(color_screen,lv_color_hex(colors[color_index]),0);
}
static void test_colors(lv_event_t *e) {
    color_index=0;color_screen=lv_obj_create(lv_layer_top());lv_obj_set_size(color_screen,360,360);lv_obj_center(color_screen);
    lv_obj_set_style_bg_color(color_screen,lv_color_hex(colors[0]),0);lv_obj_set_style_border_width(color_screen,0,0);
    lv_obj_add_event_cb(color_screen,next_color,LV_EVENT_CLICKED,NULL);
    lv_obj_t *l=lv_label_create(color_screen);lv_label_set_text(l,"点击切换颜色 / 返回");lv_obj_set_style_text_font(l,&font_music_cjk,0);lv_obj_center(l);lv_obj_set_style_text_color(l,lv_color_hex(0x808080),0);
}
void Test_UI_ShowHome(void) { if (overlay) { lv_obj_del(overlay); overlay=editor=keyboard=edit_target=NULL; } if(color_screen) {lv_obj_del(color_screen);color_screen=NULL;} show_page(0); }

static void show_page(int id) {
    Media_Hide();page=id; if(id==7) { Music_Show(); return; } if(id>=8) {Media_Show(id==9);return;} lv_scr_load(lab_screen); lv_obj_set_style_pad_row(body,id?12:8,0);lv_obj_clean(body);lv_obj_scroll_to_y(body,0,LV_ANIM_OFF);
    values=bar=chart=ssid=password=net=aps=logview=sendtext=baud=hexbox=crlf=NULL;series=NULL;
    const char *names[]={"设备实验室","内存","运动传感器","串口调试","无线网络","音频测试","板载外设","音乐播放"};
    lv_label_set_text(title,names[id]);
    lv_obj_set_x(title,id?135:100);
    if(id) lv_obj_clear_flag(back,LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(back,LV_OBJ_FLAG_HIDDEN);
    if(id==0) {

        lv_obj_t *grid=lv_obj_create(body);lv_obj_set_size(grid,LV_PCT(100),LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(grid,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(grid,0,0);lv_obj_set_style_pad_all(grid,0,0);lv_obj_set_style_pad_row(grid,6,0);lv_obj_set_style_pad_column(grid,8,0);lv_obj_set_flex_flow(grid,LV_FLEX_FLOW_ROW_WRAP);lv_obj_clear_flag(grid,LV_OBJ_FLAG_SCROLLABLE);
        const char *items[]={"内存闪存","运动传感","串口调试","无线网络","音频测试","板载外设","音乐播放","照片查看","视频播放"};
        for(int i=0;i<9;i++) {lv_obj_t *b=button(grid,items[i],navigate,(void *)(intptr_t)(i+1));lv_obj_set_size(b,76,50);lv_obj_set_style_pad_all(b,0,0);lv_obj_set_style_text_color(b,lv_color_hex(ACCENT),0);}
    } else if(id==1) {
        hint(body,"实时容量与内存分配");values=label(body,"");bar=lv_bar_create(body);lv_obj_set_size(bar,LV_PCT(100),10);
        hint(body,"PSRAM 进度条 = 已用 / 总容量\n闪存分区为预留空间，未挂载闪存文件系统。");
    } else if(id==2) {
        hint(body,"QMI8658  /  10 赫兹");values=label(body,"");
        chart=lv_chart_create(body);lv_obj_set_style_bg_color(chart,lv_color_hex(CARD),0);lv_obj_set_style_border_width(chart,0,0);lv_obj_set_style_size(chart,0,LV_PART_INDICATOR);lv_obj_set_size(chart,LV_PCT(100),90);lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,-64,64);lv_chart_set_point_count(chart,64);
        series=lv_chart_add_series(chart,lv_color_hex(ACCENT),LV_CHART_AXIS_PRIMARY_Y);
        hint(body,"曲线：陀螺仪 Z 轴（度/秒）\n旋转或倾斜开发板进行测试。");
    } else if(id==3) {
        hint(body,"发送 GPIO43  /  接收 GPIO44  /  8N1");net=label(body,uart_ok?"已就绪":"串口初始化失败");
        baud=lv_dropdown_create(body);lv_obj_add_event_cb(baud,dropdown_clicked,LV_EVENT_CLICKED,NULL);lv_obj_set_style_bg_color(baud,lv_color_hex(CARD),0);lv_obj_set_style_max_height(lv_dropdown_get_list(baud),112,0);lv_obj_set_width(baud,LV_PCT(100));lv_dropdown_set_options(baud,"9600\n19200\n38400\n57600\n115200\n230400\n460800\n921600");lv_dropdown_set_selected(baud,4);lv_obj_add_event_cb(baud,change_baud,LV_EVENT_VALUE_CHANGED,NULL);
        if(uart_ok) {uint32_t rate;uart_get_baudrate(UART_NUM_1,&rate);const int rates[]={9600,19200,38400,57600,115200,230400,460800,921600};for(int i=0;i<8;i++) if(rate==rates[i]) lv_dropdown_set_selected(baud,i);}
        values=label(body,"");logview=lv_textarea_create(body);lv_obj_set_size(logview,LV_PCT(100),110);lv_textarea_set_text(logview,uart_log);lv_obj_set_style_bg_color(logview,lv_color_hex(CARD),0);lv_obj_set_style_text_font(logview,&font_music_cjk,0);
        hexbox=lv_checkbox_create(body);lv_checkbox_set_text(hexbox,"十六进制收发");
        crlf=lv_checkbox_create(body);lv_checkbox_set_text(crlf,"追加回车换行");
        sendtext=input(body,"发送数据",96,false);button(body,"发送",send_uart,NULL);button(body,"清空日志和计数",clear_uart,NULL);
        hint(body,"3.3V 串口：TX/RX 交叉连接并共地；短接 43 与 44 可测试回环");
    } else if(id==4) {
        hint(body,"2.4 GHz 无线网络");net=label(body,snapshot.network);button(body,"扫描网络",scan,NULL);
        aps=lv_dropdown_create(body);lv_obj_add_event_cb(aps,dropdown_clicked,LV_EVENT_CLICKED,NULL);lv_obj_set_style_bg_color(aps,lv_color_hex(CARD),0);lv_obj_set_style_max_height(lv_dropdown_get_list(aps),112,0);lv_dropdown_set_dir(aps,LV_DIR_TOP);lv_obj_set_width(aps,LV_PCT(100));lv_dropdown_set_options(aps,"请先扫描");lv_obj_add_event_cb(aps,select_ap,LV_EVENT_VALUE_CHANGED,NULL);ap_version=UINT32_MAX;
        ssid=input(body,"网络名称（SSID）",32,false);password=input(body,"密码",63,true);
        button(body,"连接",connect_wifi,NULL);
        hint(body,"点击输入框打开键盘；开放网络可不填密码；密码只保存在内存中。");
    } else if(id==5) {
        hint(body,"I2S 麦克风  /  16 kHz");values=label(body,"");bar=lv_bar_create(body);lv_obj_set_size(bar,LV_PCT(100),10);
        chart=lv_chart_create(body);lv_obj_set_style_bg_color(chart,lv_color_hex(CARD),0);lv_obj_set_style_border_width(chart,0,0);lv_obj_set_style_size(chart,0,LV_PART_INDICATOR);lv_obj_set_size(chart,LV_PCT(100),86);lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,-100,100);lv_chart_set_point_count(chart,64);
        series=lv_chart_add_series(chart,lv_color_hex(ACCENT),LV_CHART_AXIS_PRIMARY_Y);
        button(body,"播放测试音",tone,NULL);
        hint(body,"电平为数字 dBFS（不是声压值）；波形已放大显示；测试音持续 0.5 秒，音量较低。");
    } else {
        hint(body,"板载外设状态");values=label(body,"");
        hint(body,"显示亮度");bar=lv_slider_create(body);lv_obj_set_width(bar,LV_PCT(95));lv_slider_set_range(bar,10,100);lv_slider_set_value(bar,LCD_Backlight,LV_ANIM_OFF);lv_obj_add_event_cb(bar,brightness,LV_EVENT_VALUE_CHANGED,NULL);
        button(body,"屏幕颜色与触摸测试",test_colors,NULL);
        button(body,"触摸校准",calibrate_touch,NULL);
        hint(body,"依次点击白、红、绿、蓝、黑色返回；SD 卡仅做只读测试，不格式化也不写入");
    }
}
static void refresh(lv_timer_t *timer) {
    static int ticks;static char last_net[96];
    Test_State(&snapshot);
    if(uart_ok) {rx_packet_t p;for(int i=0;i<16 && xQueueReceive(rx_packets,&p,0);i++) {append_log("RX",p.bytes,p.n);}}
    if(++ticks%10==0) {
        if(snapshot.rtc_ok) label_fmt(footer,"%02u:%02u:%02u / %s",snapshot.hour,snapshot.minute,snapshot.second,snapshot.connected?"在线":"离线");
        else label_fmt(footer,"RTC --:--:-- / %s",snapshot.connected?"在线":"离线");
    }
    if(page==0 && values) label_fmt(values,"PSRAM 可用  %.2f MB\n电池  %.2f V   |   %s",heap_caps_get_free_size(MALLOC_CAP_SPIRAM)/1048576.0,snapshot.battery,snapshot.imu_ok?"陀螺仪正常":"陀螺仪异常");
    if(page==1) {
        size_t total=heap_caps_get_total_size(MALLOC_CAP_SPIRAM),free=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        label_fmt(values,"FLASH 总容量  %.2f MB\n分区预留  %.2f MB\n\nPSRAM 可用  %.2f / %.2f MB\n最大连续块  %.2f MB\n内部内存可用  %u KB\n运行时间  %llu 秒",flash_bytes/1048576.0,partition_bytes/1048576.0,free/1048576.0,total/1048576.0,heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)/1048576.0,(unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)/1024),(unsigned long long)(esp_timer_get_time()/1000000));
        lv_bar_set_value(bar,total?(total-free)*100/total:0,LV_ANIM_OFF);
    }
    if(page==2) {
        label_fmt(values,"%s\n加速度（g）\nX %+.3f   Y %+.3f   Z %+.3f\n角速度（度/秒）\nX %+.2f   Y %+.2f   Z %+.2f",snapshot.imu_ok?"传感器在线":"传感器读取失败",snapshot.accel[0],snapshot.accel[1],snapshot.accel[2],snapshot.gyro[0],snapshot.gyro[1],snapshot.gyro[2]);
        lv_chart_set_next_value(chart,series,(lv_coord_t)snapshot.gyro[2]);
    }
    if(page==3) {if(strcmp(lv_textarea_get_text(logview),uart_log)) {lv_textarea_set_text(logview,uart_log);lv_textarea_set_cursor_pos(logview,LV_TEXTAREA_CURSOR_LAST);}
    uint64_t rx,lost;unsigned errors;
    portENTER_CRITICAL(&uart_guard);rx=received;lost=dropped;errors=uart_errors;portEXIT_CRITICAL(&uart_guard);
    label_fmt(values,"接收 %llu 字节 / 发送 %llu 字节\n丢失日志 %llu / 串口错误 %u",(unsigned long long)rx,(unsigned long long)sent,(unsigned long long)lost,errors);}
    if(page==4) {
        if(strcmp(lv_label_get_text(net),snapshot.network)) {lv_label_set_text(net,snapshot.network);snprintf(last_net,sizeof(last_net),"%s",snapshot.network);}
        if(ap_version!=snapshot.scan_version) {
            ap_version=snapshot.scan_version;char options[800]="";
            for(int i=0;i<snapshot.aps;i++) {
                char clean[33];snprintf(clean,sizeof(clean),"%s",snapshot.ssids[i]);for(char *c=clean;*c;c++) if((unsigned char)*c<32)*c='?';
                size_t len=strlen(options);snprintf(options+len,sizeof(options)-len,"%s%s (%d)",i?"\n":"",clean[0]?clean:"隐藏网络",snapshot.rssi[i]);
            }
            bool was_open=lv_dropdown_is_open(aps);
            lv_dropdown_close(aps);
            lv_dropdown_set_options(aps,options[0]?options:"未发现网络");
            if(was_open) { lv_dropdown_open(aps);position_dropdown(aps); }
            if(snapshot.aps && !strlen(lv_textarea_get_text(ssid))) lv_textarea_set_text(ssid,snapshot.ssids[0]);
        }
    }
    if(page==5) {
        label_fmt(values,"%s  /  %.1f dBFS\n扬声器：%s",snapshot.mic_ok?"麦克风采集中":"麦克风不可用",snapshot.mic_db,snapshot.speaker_ok?"就绪":"不可用");
        lv_bar_set_value(bar,(int)fmaxf(0,fminf(100,(snapshot.mic_db+90)*100/90)),LV_ANIM_OFF);
        for(int i=0;i<64;i++) {series->y_points[i]=snapshot.wave[i];}
        lv_chart_refresh(chart);
    }
    if(page==6) label_fmt(values,"RTC %s  %02u:%02u:%02u\n电池  %.3f V\n电源按键  %s\nSD 卡  %s（%lu MB）\n屏幕 360 x 360 / RGB565\n触摸芯片 CST816",snapshot.rtc_ok?"正常":"异常",snapshot.hour,snapshot.minute,snapshot.second,snapshot.battery,snapshot.key?"释放":"按下",SDCard_Size?"已挂载":"未挂载",(unsigned long)SDCard_Size);
}
void Test_UI_Init(void) {
    usb_serial_jtag_driver_config_t usb_cfg={.tx_buffer_size=4096,.rx_buffer_size=4096};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_cfg));
    usb_serial_jtag_vfs_use_driver();
    lv_theme_default_init(lv_disp_get_default(),lv_color_hex(ACCENT),lv_color_hex(0x7187FF),true,&font_music_cjk);
    lv_obj_t *screen=lv_obj_create(NULL); lab_screen=screen; lv_scr_load(screen);lv_obj_set_style_bg_color(screen,lv_color_hex(BG),0);lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);lv_obj_set_style_text_color(screen,lv_color_hex(0xE8F0FC),0);lv_obj_set_style_text_font(screen,&font_music_cjk,0);
    title=lv_label_create(screen);lv_obj_set_style_text_font(title,&font_music_cjk,0);lv_obj_set_width(title,160);lv_obj_set_style_text_align(title,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_pos(title,135,53);
    back=lv_btn_create(screen);lv_obj_set_size(back,64,44);lv_obj_set_pos(back,66,39);lv_obj_set_style_bg_color(back,lv_color_hex(CARD),0);lv_obj_t *l=lv_label_create(back);lv_label_set_text(l,"返回");lv_obj_set_style_text_font(l,&font_music_cjk,0);lv_obj_center(l);lv_obj_add_event_cb(back,navigate,LV_EVENT_CLICKED,(void *)(intptr_t)0);
    body=lv_obj_create(screen);lv_obj_set_pos(body,52,90);lv_obj_set_size(body,256,194);lv_obj_set_style_bg_opa(body,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(body,0,0);lv_obj_set_style_pad_all(body,5,0);lv_obj_set_style_pad_row(body,12,0);lv_obj_set_flex_flow(body,LV_FLEX_FLOW_COLUMN);lv_obj_set_scroll_dir(body,LV_DIR_VER);
    footer=lv_label_create(screen);lv_obj_set_style_text_color(footer,lv_color_hex(MUTED),0);lv_obj_set_style_text_font(footer,&font_music_cjk,0);lv_label_set_text(footer,"设备实验室 / 正在启动");lv_obj_align(footer,LV_ALIGN_BOTTOM_MID,0,-38);
    esp_flash_get_size(NULL,&flash_bytes);
    esp_partition_iterator_t it=esp_partition_find(ESP_PARTITION_TYPE_ANY,ESP_PARTITION_SUBTYPE_ANY,NULL);
    while(it) {partition_bytes+=esp_partition_get(it)->size;it=esp_partition_next(it);}
    uart_config_t cfg={.baud_rate=115200,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,.stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    esp_err_t err=uart_param_config(UART_NUM_1,&cfg);
    if(err==ESP_OK) err=uart_set_pin(UART_NUM_1,43,44,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
    if(err==ESP_OK) err=uart_driver_install(UART_NUM_1,8192,0,20,&uart_events,0);
    rx_packets=xQueueCreate(32,sizeof(rx_packet_t));
    uart_ok=err==ESP_OK && rx_packets;
    if(uart_ok) uart_ok=xTaskCreate(uart_worker,"test_uart",3072,NULL,4,NULL)==pdPASS;
    gpio_config_t key_cfg={.pin_bit_mask=(1ULL<<KEY_MUSIC)|(1ULL<<KEY_PREV)|(1ULL<<KEY_NEXT),.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,.pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    gpio_config(&key_cfg);
    Music_Set_Back_Callback(music_back);
    Media_Init(Test_UI_ShowHome);
    Test_State(&snapshot);show_page(0);lv_timer_create(refresh,100,NULL);
}

static unsigned audit_text(const char *text,const lv_font_t *font) {
    unsigned missing=0;uint32_t offset=0;
    while(text[offset]) {
        uint32_t c=_lv_txt_encoded_next(text,&offset);
        if(c<32) continue;
        lv_font_glyph_dsc_t glyph;
        if(!lv_font_get_glyph_dsc(font,&glyph,c,0) || glyph.is_placeholder) {
            printf("LAB missing U+%04lX\n",(unsigned long)c);missing++;
        }
    }
    return missing;
}
static unsigned audit_tree(lv_obj_t *obj) {
    if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN)) return 0;
    unsigned missing=0;
    if(lv_obj_check_type(obj,&lv_label_class))
        missing+=audit_text(lv_label_get_text(obj),lv_obj_get_style_text_font(obj,LV_PART_MAIN));
    if(lv_obj_check_type(obj,&lv_textarea_class)) {
        missing+=audit_text(lv_textarea_get_text(obj),lv_obj_get_style_text_font(obj,LV_PART_MAIN));
        const char *placeholder=lv_textarea_get_placeholder_text(obj);
        if(placeholder) missing+=audit_text(placeholder,lv_obj_get_style_text_font(obj,LV_PART_MAIN));
    }
    for(unsigned i=0;i<lv_obj_get_child_cnt(obj);i++) missing+=audit_tree(lv_obj_get_child(obj,i));
    return missing;
}
/* USB diagnostics run on the LVGL owner task. GPIO43/44 remain a separate UART. */
void Test_UI_Debug_Poll(void) {
    bool key1=gpio_get_level(KEY_MUSIC),key2=gpio_get_level(KEY_PREV),key3=gpio_get_level(KEY_NEXT);
    if(key1_last && !key1) { if(page==7) Test_UI_ShowHome(); else show_page(7); }
    if(page>=8) {if(key2_last && !key2)Media_Next(-1);if(key3_last && !key3)Media_Next(1);}
    else if(page==7) { if(key2_last && !key2) Music_Prev(); if(key3_last && !key3) Music_Next(); }
    else if(key2_last && !key2) show_page(page>0?page-1:0);
    else if(key3_last && !key3) show_page(page<9?page+1:0);
    key1_last=key1;key2_last=key2;key3_last=key3;
    static char command[1200];static unsigned used;
    EXT_RAM_BSS_ATTR static char incoming[4096];int n=usb_serial_jtag_read_bytes(incoming,sizeof(incoming),0);
    for(int i=0;i<n;i++) {
        size_t binary=Media_Debug_Binary((uint8_t *)incoming+i,n-i);
        if(binary) {i+=(int)binary-1;continue;}
        char c=incoming[i];
        if(c=='\r') continue;
        if(c!='\n') {if(used<sizeof(command)-1) command[used++]=c;continue;}
        command[used]=0;used=0;
        int value,tap_y,cy,cm,cd,ch,cmin,cs,cw;
        if(Media_Debug_Upload(command)) {
        } else if(!strcmp(command,"media-files")) {Media_Debug_Files();
        } else if(!strcmp(command,"media-status")) {Media_Debug_Status();
        } else if(!strcmp(command,"media-prepare")) {Media_Prepare();
        } else if(!strcmp(command,"media-scan")) {show_page(8);Media_Rescan();
        } else if(sscanf(command,"media-open %d",&value)==1 && value>=0) {Media_Debug_Open(value);
        } else if(sscanf(command,"media-next %d",&value)==1) {Media_Next(value);
        } else if(sscanf(command,"media-list-page %d",&value)==1) {Media_Debug_List_Page(value);
        } else if(!strcmp(command,"media-toggle")) {Media_Toggle();
        } else if(!strcmp(command,"media-back")) {Media_Back();
        } else if(sscanf(command,"clock %d %d %d %d %d %d %d",&cy,&cm,&cd,&ch,&cmin,&cs,&cw)==7
           && cy>=2000 && cy<=2099 && cm>=1 && cm<=12 && cd>=1 && cd<=31
           && ch>=0 && ch<24 && cmin>=0 && cmin<60 && cs>=0 && cs<60 && cw>=0 && cw<7) {
            datetime_t value={.year=cy,.month=cm,.day=cd,.hour=ch,.minute=cmin,.second=cs,.dotw=cw};
            PCF85063_Set_All(value); puts("LAB clock set");
        } else if(sscanf(command,"touch-raw %d %d %d",&value,&tap_y,&cy)==3) {
            LVGL_Debug_Raw_Touch(value,tap_y,cy!=0);puts("LAB injected raw touch");
        } else if(!strcmp(command,"touch-cal")) {
            Touch_Calibration_Start();
        } else if(!strcmp(command,"touch-cancel")) {
            Touch_Calibration_Cancel();
        } else if(!strcmp(command,"touch-stats")) {
            LVGL_Touch_Status();
        } else if(sscanf(command,"touch-trace %d",&value)==1 && value>=0 && value<=180) {
            LVGL_Touch_Trace((unsigned)value);puts("LAB touch trace set");
        } else if(sscanf(command,"tap %d %d",&value,&tap_y)==2) {
            LVGL_Debug_Tap(value,tap_y); puts("LAB tap queued");
        } else if(!strncmp(command,"join ",5)) {
            char *separator=strchr(command+5,'\t');
            if(separator) {*separator=0;printf("LAB join %s\n",esp_err_to_name(Test_Wifi_Connect(command+5,separator+1)));}
            else puts("LAB join expects SSID<TAB>password");
            memset(command,0,sizeof(command));
        } else if(sscanf(command,"play %d",&value)==1 && value>=0 && value<Music_Track_Count) {
            Music_Play_Track((uint16_t)value); printf("LAB play %d requested\n",value);
        } else if(!strcmp(command,"status")) {
            printf("LAB screen=%s\n",lv_scr_act()==lab_screen?"lab":page>=8?"media":"music");
            printf("LAB audio tracks=%u bytes=%lu ready=%d\n",Music_Track_Count,(unsigned long)Audio_Bytes_Written,Audio_Ready());
            test_state_t s;Test_State(&s);
            printf("LAB status page=%d flash=%lu psram_free=%u heap=%u imu=%d accel=%.3f,%.3f,%.3f gyro=%.3f,%.3f,%.3f mic=%d db=%.1f speaker=%d rtc=%d battery=%.3f sd_mb=%lu wifi=%s\n",page,(unsigned long)flash_bytes,(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),s.imu_ok,s.accel[0],s.accel[1],s.accel[2],s.gyro[0],s.gyro[1],s.gyro[2],s.mic_ok,s.mic_db,s.speaker_ok,s.rtc_ok,s.battery,(unsigned long)SDCard_Size,s.network);
            esp_netif_t *iface=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if(iface) {esp_netif_ip_info_t info={0};esp_netif_dhcp_status_t dhcp;esp_netif_get_ip_info(iface,&info);esp_netif_dhcpc_get_status(iface,&dhcp);printf("LAB netif up=%d dhcp=%d ip=" IPSTR "\n",esp_netif_is_netif_up(iface),dhcp,IP2STR(&info.ip));}
        } else if(sscanf(command,"page %d",&value)==1 && value>=0 && value<=9 && !overlay) {
            show_page(value);printf("LAB page %d\n",value);
        } else if(sscanf(command,"scroll %d",&value)==1) {
            lv_obj_scroll_to_y(body,value,LV_ANIM_OFF);puts("LAB scroll");
        } else if(!strcmp(command,"audit")) {
            printf("LAB audit missing=%u gui_stack_free=%u\n",audit_tree(overlay?overlay:lv_scr_act()),(unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else if(!strcmp(command,"tracks")) {
            Music_Dump_Tracks();
        } else if(!strcmp(command,"music-list")) {
            Music_Show_List(); puts("LAB music list");
        } else if(sscanf(command,"list-page %d",&value)==1) {
            Music_List_Page(value); puts("LAB list page");
        } else if(!strcmp(command,"music-toggle")) {
            Music_Play_Pause_Toggle();
        } else if(!strcmp(command,"back")) {
            Test_UI_ShowHome();
        } else if(!strcmp(command,"pause")) {
            Music_Pause(); puts("LAB paused");
        } else if(!strcmp(command,"scan")) {
            printf("LAB scan %s\n",esp_err_to_name(Test_Wifi_Scan()));
        } else if(!strcmp(command,"tone")) {
            Test_Tone();puts("LAB tone");
        } else if(!strcmp(command,"edit") && (ssid || sendtext)) {
            lv_event_send(ssid?ssid:sendtext,LV_EVENT_CLICKED,NULL);puts("LAB editor");
        } else if(!strcmp(command,"cancel") && keyboard) {
            lv_event_send(keyboard,LV_EVENT_CANCEL,NULL);puts("LAB cancel");
        } else if(!strcmp(command,"shot")) {
            lv_obj_t *obj=overlay?overlay:color_screen?color_screen:lv_scr_act();
            lv_obj_update_layout(obj);
            uint32_t bytes=lv_snapshot_buf_size_needed(obj,LV_IMG_CF_TRUE_COLOR);
            void *buffer=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);lv_img_dsc_t image;
            if(buffer && lv_snapshot_take_to_buf(obj,LV_IMG_CF_TRUE_COLOR,&image,buffer,bytes)==LV_RES_OK) {
                printf("LAB SHOT %u %u %u\n",image.header.w,image.header.h,LV_COLOR_16_SWAP);
                const uint8_t *pixels=image.data;
                for(int y=0;y<image.header.h;y++) {
                    char row[1441];
                    for(int x=0;x<image.header.w*2;x++) {static const char digits[]="0123456789ABCDEF";uint8_t b=pixels[y*image.header.w*2+x];row[x*2]=digits[b>>4];row[x*2+1]=digits[b&15];}
                    row[image.header.w*4]=0;puts(row);vTaskDelay(1);
                }
                puts("LAB END SHOT");
            } else puts("LAB snapshot failed");
            free(buffer);
        } else puts("LAB commands: status, page 0..7, scroll N, scan, tone, edit, cancel, shot");
    }
}
