#include "LVGL_Driver.h"
#include "Touch_Calibration.h"

static const char *TAG_LVGL = "LVGL";

    

lv_disp_draw_buf_t disp_buf;                                                 // contains internal graphic buffer(s) called draw buffer(s)
lv_disp_drv_t disp_drv;                                                      // contains callback functions
lv_indev_drv_t indev_drv;

void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}


void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 +1, offsety2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

/* USB test input exercises LVGL hit testing without bypassing its input path. */
static bool debug_touch_active;
static uint32_t debug_touch_until;
static lv_point_t debug_touch_point;
static bool debug_touch_raw;
static lv_indev_t *touch_indev;
static uint32_t touch_reads,touch_errors,touch_invalid,touch_downs,touch_gap_max,touch_last_ms,touch_good_ms;
static uint32_t touch_trace_until,touch_trace_last;
static bool touch_last_down;
static lv_point_t touch_last_point;
void LVGL_Touch_Trace(unsigned seconds) {touch_trace_until=lv_tick_get()+seconds*1000;touch_gap_max=0;}
void LVGL_Touch_Status(void) {
    lv_point_t processed={0};if(touch_indev)lv_indev_get_point(touch_indev,&processed);
    printf("LAB touch reads=%lu errors=%lu invalid=%lu presses=%lu max_gap_ms=%lu logical=%d,%d down=%d\n",
           (unsigned long)touch_reads,(unsigned long)touch_errors,(unsigned long)touch_invalid,
           (unsigned long)touch_downs,(unsigned long)touch_gap_max,touch_last_point.x,touch_last_point.y,touch_last_down);
    printf("LAB touch LVGL=%d,%d rotation=%d period_ms=%lu\n",processed.x,processed.y,disp_drv.rotated,(unsigned long)touch_indev->driver->read_timer->period);
}
void LVGL_Debug_Tap(int x,int y) {
    if(x<0 || x>=360 || y<0 || y>=360) return;
    debug_touch_point.x=x; debug_touch_point.y=y;
    debug_touch_raw=false;
    debug_touch_until=lv_tick_get()+120;debug_touch_active=true;
}
void LVGL_Debug_Raw_Touch(int x,int y,bool pressed) {
    if(x<0 || x>=360 || y<0 || y>=360)return;
    debug_touch_point.x=x;debug_touch_point.y=y;debug_touch_raw=true;
    debug_touch_until=lv_tick_get()+(pressed?1000:0);debug_touch_active=true;
}
/*Read the touchpad*/
void example_touchpad_read( lv_indev_drv_t * drv, lv_indev_data_t * data )
{
    if(debug_touch_active) {
        data->point=debug_touch_point;
        if(debug_touch_raw) Touch_Map(debug_touch_point.x,debug_touch_point.y,&data->point);
        data->state=(int32_t)(debug_touch_until-lv_tick_get())>0 ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
        if(data->state==LV_INDEV_STATE_REL) debug_touch_active=false;
        return;
    }
    uint16_t touchpad_x[5] = {0};
    uint16_t touchpad_y[5] = {0};
    uint8_t touchpad_cnt = 0;

    uint32_t now=lv_tick_get();
    if(touch_last_ms && now-touch_last_ms>touch_gap_max) touch_gap_max=now-touch_last_ms;
    touch_last_ms=now;touch_reads++;
    esp_err_t err=esp_lcd_touch_read_data(drv->user_data);
    if(err!=ESP_OK) {
        touch_errors++;
        /* A brief bus error must not split one physical press into two clicks. */
        data->point=touch_last_point;
        data->state=touch_last_down && now-touch_good_ms<40?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
        if(data->state==LV_INDEV_STATE_REL) touch_last_down=false;
        return;
    }
    touch_good_ms=now;

    /* Get coordinates */
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(drv->user_data, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 5);

    bool pressed=touchpad_pressed && touchpad_cnt>0;
    if(pressed && (touchpad_x[0]>=360 || touchpad_y[0]>=360)) {touch_invalid++;pressed=false;}
    if(pressed) Touch_Map(touchpad_x[0],touchpad_y[0],&touch_last_point);
    if(pressed && !touch_last_down) touch_downs++;
    if((int32_t)(touch_trace_until-now)>0 &&
       (pressed!=touch_last_down || (pressed && now-touch_trace_last>=100))) {
        printf("LAB touch physical=%d raw=%u,%u logical=%d,%d ms=%lu\n",pressed,touchpad_x[0],touchpad_y[0],touch_last_point.x,touch_last_point.y,(unsigned long)now);
        touch_trace_last=now;
    }
    touch_last_down=pressed;
    /* Panel and LVGL both use native orientation; never swap these axes. */
    data->point=touch_last_point;
    data->state=pressed?LV_INDEV_STATE_PR:LV_INDEV_STATE_REL;
    if(Touch_Calibration_Sample(pressed,touchpad_x[0],touchpad_y[0])) data->state=LV_INDEV_STATE_REL;
   
}
/* Rotate display and touch, when rotated screen in LVGL. Called when driver parameters are updated. */
void example_lvgl_port_update_callback(lv_disp_drv_t *drv)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;

    switch (drv->rotated) {
    case LV_DISP_ROT_NONE:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, false);
        break;
    case LV_DISP_ROT_90:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, true);
        break;
    case LV_DISP_ROT_180:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, true);
        break;
    case LV_DISP_ROT_270:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, false);
        break;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
lv_disp_t *disp;
void LVGL_Init(void)
{
    ESP_LOGI(TAG_LVGL, "Initialize LVGL library");
    lv_init();
    
    /* Reserve DMA buffers at startup; PSRAM buffers caused per-transfer internal
     * allocations to fail when video and audio were active together. */
    lv_color_t *buf1 = heap_caps_malloc(LVGL_BUF_LEN * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = heap_caps_malloc(LVGL_BUF_LEN * sizeof(lv_color_t) , MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    assert(buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LVGL_BUF_LEN);                              // initialize LVGL draw buffers

    ESP_LOGI(TAG_LVGL, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);                                                                        // Create a new screen object and initialize the associated device
    disp_drv.hor_res = EXAMPLE_LCD_WIDTH;             
    disp_drv.ver_res = EXAMPLE_LCD_HEIGHT;                                                     // Horizontal pixel count
    /* lv_disp_drv_register does not call drv_update_cb. Setting ROT_90 here
     * rotated only input coordinates while the panel stayed in native orientation. */
    disp_drv.rotated = LV_DISP_ROT_NONE;
    disp_drv.flush_cb = example_lvgl_flush_cb;                                                          // Function : copy a buffer's content to a specific area of the display
    disp_drv.drv_update_cb = example_lvgl_port_update_callback;                                         // Function : Rotate display and touch, when rotated screen in LVGL. Called when driver parameters are updated. 
    disp_drv.draw_buf = &disp_buf;                                                                      // LVGL will use this buffer(s) to draw the screens contents
    disp_drv.user_data = panel_handle;                
    ESP_LOGI(TAG_LVGL,"Register display indev to LVGL");                                                  // Custom display driver user data
    disp = lv_disp_drv_register(&disp_drv);     
    
    lv_indev_drv_init ( &indev_drv );
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = example_touchpad_read;
    indev_drv.user_data = tp;
    indev_drv.scroll_limit=16;
    touch_indev=lv_indev_drv_register( &indev_drv );
    lv_timer_set_period(touch_indev->driver->read_timer,10);

    /********************* LVGL *********************/
    ESP_LOGI(TAG_LVGL, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

}
