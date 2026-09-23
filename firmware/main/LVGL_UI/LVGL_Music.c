#include "LVGL_Music.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"

/* 自定义中文字体（包含所有歌名汉字） */
extern const lv_font_t font_music_cjk;

/*********************
 *      DEFINES
 *********************/
#define SCREEN_W            360
#define SCREEN_H            360
#define DISC_SIZE           110
#define EQ_BAR_CNT          5
#define EQ_BAR_W            9
#define VOLUME_HIDE_MS      3000

/**********************
 *  STATIC VARIABLES
 **********************/
static lv_obj_t *screen_play;
static void (*music_back_callback)(void) = NULL;
static lv_obj_t *screen_list;

static lv_obj_t *label_title,*quality_label;
static lv_obj_t *disc_arc;
static lv_obj_t *eq_bars[EQ_BAR_CNT];
static lv_obj_t *btn_play;
static lv_obj_t *label_play_icon;
static lv_obj_t *label_time;
static lv_obj_t *slider_volume;
static lv_obj_t *label_volume_icon;
static lv_obj_t *list_cont,*list_page_label;
static unsigned list_page;
#define TRACKS_PER_PAGE 12

EXT_RAM_BSS_ATTR static char sd_names[MUSIC_MAX_TRACKS][MUSIC_NAME_LEN];
EXT_RAM_BSS_ATTR static char disp_names[MUSIC_MAX_TRACKS][MUSIC_NAME_LEN];
static uint16_t cur_track = 0;
uint16_t Music_Track_Count = 0;
static bool is_playing = false;
static bool track_loaded = false;
static uint32_t play_start_tick = 0;
static uint32_t paused_elapsed = 0;
static int16_t disc_angle = 0;
static uint8_t eq_phase = 0;
static lv_timer_t *main_timer;
static lv_timer_t *vol_hide_timer;

/**********************
 *  STATIC FUNCTIONS
 **********************/
static void scan_sd_card(void);
static void scan_dir(const char *dir_path);
static void remove_path_and_ext(char *name);
static void start_play(uint16_t index);
static void toggle_play_pause(void);
static void play_next(void);
static void play_prev(void);
static void update_ui_state(void);
static void format_time(char *buf, uint32_t sec);

static void event_btn_list(lv_event_t *e);
static void event_btn_back(lv_event_t *e);
static void event_btn_play(lv_event_t *e);
static void event_btn_prev(lv_event_t *e);
static void event_btn_next(lv_event_t *e);
static void event_btn_volume(lv_event_t *e);
static void event_slider_volume(lv_event_t *e);
static void event_list_item(lv_event_t *e);
static void timer_cb(lv_timer_t *t);
static void vol_hide_cb(lv_timer_t *t);
static void reset_vol_hide(void);

static void create_play_screen(void);
static void create_list_screen(void);
static void refresh_list(void);
static lv_obj_t *create_ctrl_btn(lv_obj_t *parent, const char *symbol, const lv_font_t *font, bool highlight, uint16_t size);

/**********************
 *   PUBLIC FUNCTIONS
 **********************/

void Music_Player_Init(void)
{
    scan_sd_card();
    create_play_screen();
    lv_scr_load(screen_play);

    if (Music_Track_Count > 0) {
        lv_label_set_text(label_title, "请选择歌曲");
        lv_label_set_text(label_time, "点击播放");
    } else {
        lv_label_set_text(label_title, "暂无音乐");
        lv_label_set_text(label_time, "--:--");
    }
    main_timer = lv_timer_create(timer_cb, 150, NULL);
}

void Music_Play_Track(uint16_t index)
{
    if (index < Music_Track_Count) start_play(index);
}

void Music_Play_Pause_Toggle(void) { toggle_play_pause(); }
void Music_Pause(void) { if(is_playing) toggle_play_pause(); }

void Music_Resume(void)
{
    if (Music_Track_Count > 0 && !is_playing) {
        Music_resume();
        is_playing = true;
        play_start_tick = lv_tick_get();
        update_ui_state();
    }
}

void Music_Next(void) { play_next(); }
void Music_Prev(void) { play_prev(); }

void Music_Set_Volume(uint8_t vol)
{
    Volume_adjustment(vol);
    if (slider_volume) lv_slider_set_value(slider_volume, vol, LV_ANIM_OFF);
}


void Music_Show_List(void) { if(!screen_list) create_list_screen(); lv_scr_load(screen_list); }

void Music_Show(void) {
    if (screen_play) lv_scr_load(screen_play);
}

void Music_Set_Back_Callback(void (*callback)(void)) {
    music_back_callback = callback;
}
/**********************
 *   STATIC FUNCTIONS
 **********************/

static void scan_dir(const char *dir_path)
{
    DIR *dir = opendir(dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && Music_Track_Count < MUSIC_MAX_TRACKS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            scan_dir(full_path);
            continue;
        }

        const char *dot = strrchr(entry->d_name, '.');
        if (dot && (strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0)) {
            const char *rel = full_path + strlen(MUSIC_SD_PATH) + 1;
            strncpy(sd_names[Music_Track_Count], rel, MUSIC_NAME_LEN - 1);
            sd_names[Music_Track_Count][MUSIC_NAME_LEN - 1] = '\0';
            Music_Track_Count++;
        }
    }
    closedir(dir);
}

static void scan_sd_card(void)
{
    Music_Track_Count = 0;
    scan_dir(MUSIC_SD_PATH);

    for (uint16_t i = 0; i < Music_Track_Count; i++) {
        strncpy(disp_names[i], sd_names[i], MUSIC_NAME_LEN - 1);
        disp_names[i][MUSIC_NAME_LEN - 1] = '\0';
        remove_path_and_ext(disp_names[i]);
    }
}

static void remove_path_and_ext(char *name)
{
    /* 只保留文件名（去掉路径） */
    char *slash = strrchr(name, '/');
    if (slash) {
        memmove(name, slash + 1, strlen(slash + 1) + 1);
    }
    char *dot = strrchr(name, '.');
    if (dot) *dot = '\0';
}

static void start_play(uint16_t index)
{
    if (index >= Music_Track_Count) return;
    int tries = 0;
    uint16_t idx = index;
    while (tries < 5 && tries < Music_Track_Count) {
        cur_track = idx;
        printf("Playing: /sdcard/%s\r\n", sd_names[idx]);
        bool ok = Play_Music(MUSIC_SD_PATH, sd_names[idx]);
        if (ok) {
            const char *ext=strrchr(sd_names[idx],'.');
            lv_label_set_text(quality_label,ext && !strcasecmp(ext,".flac")?"FLAC 音乐":"MP3 音乐");
            track_loaded = true;
            is_playing = true;
            play_start_tick = lv_tick_get();
            paused_elapsed = 0;
            lv_label_set_text(label_title, disp_names[idx]);
            update_ui_state();
            refresh_list();
            return;
        }
        printf("Track %d failed, trying next...\r\n", idx);
        idx = (idx + 1) % Music_Track_Count;
        tries++;
    }
    printf("All tracks failed to play\r\n");
    is_playing = false;
    lv_label_set_text(label_title, "无法播放");
    update_ui_state();
}

static void toggle_play_pause(void)
{
    if (Music_Track_Count == 0) return;
    if (!track_loaded) { start_play(cur_track); return; }
    if (is_playing) {
        Music_pause();
        is_playing = false;
        paused_elapsed += (lv_tick_get() - play_start_tick) / 1000;
    } else {
        Music_resume();
        is_playing = true;
        play_start_tick = lv_tick_get();
    }
    update_ui_state();
}

static void play_next(void)
{
    if (Music_Track_Count == 0) return;
    start_play((cur_track + 1) % Music_Track_Count);
}

static void play_prev(void)
{
    if (Music_Track_Count == 0) return;
    uint16_t prev = (cur_track == 0) ? Music_Track_Count - 1 : cur_track - 1;
    start_play(prev);
}

static void update_ui_state(void)
{
    lv_label_set_text(label_play_icon, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void format_time(char *buf, uint32_t sec)
{
    snprintf(buf, 16, "%lu:%02lu", sec / 60, sec % 60);
}

static void timer_cb(lv_timer_t *t)
{
    if (Music_Next_Flag) {
        Music_Next_Flag = 0;
        play_next();
        return;
    }
    if (!is_playing || Music_Track_Count == 0) return;

    /* 坏文件检测：播放3秒后若I2S写入字节极少，说明解码失败，自动跳过 */
    if (lv_tick_get() - play_start_tick > 3000 && Audio_Bytes_Written < 50000) {
        printf("Bad MP3 detected (bytes=%lu), skipping track %d...\r\n", (unsigned long)Audio_Bytes_Written, cur_track);
        play_next();
        return;
    }

    disc_angle = (disc_angle + 4) % 360;
    lv_arc_set_start_angle(disc_arc, disc_angle);
    lv_arc_set_end_angle(disc_arc, (disc_angle + 70) % 360);

    eq_phase = (eq_phase + 1) % 8;
    for (int i = 0; i < EQ_BAR_CNT; i++) {
        int h = 5 + ((eq_phase + i * 3) % 5) * 4;
        lv_obj_set_height(eq_bars[i], h);
    }

    uint32_t elapsed = paused_elapsed + (lv_tick_get() - play_start_tick) / 1000;
    char buf[16];
    format_time(buf, elapsed);
    lv_label_set_text(label_time, buf);
}

static void vol_hide_cb(lv_timer_t *t)
{
    lv_obj_add_flag(slider_volume, LV_OBJ_FLAG_HIDDEN);
    if (vol_hide_timer) { lv_timer_del(vol_hide_timer); vol_hide_timer = NULL; }
}

static void reset_vol_hide(void)
{
    if (vol_hide_timer) lv_timer_del(vol_hide_timer);
    vol_hide_timer = lv_timer_create(vol_hide_cb, VOLUME_HIDE_MS, NULL);
}

/**********************
 *   EVENT CALLBACKS
 **********************/

static void event_btn_list(lv_event_t *e)  { if (!screen_list) create_list_screen(); lv_scr_load(screen_list); }
static void event_btn_back(lv_event_t *e)  { if(lv_scr_act()==screen_list) {lv_scr_load(screen_play);return;} if (music_back_callback) music_back_callback(); else lv_scr_load(screen_play); }
static void event_btn_play(lv_event_t *e)  { toggle_play_pause(); }
static void event_btn_prev(lv_event_t *e)  { play_prev(); }
static void event_btn_next(lv_event_t *e)  { play_next(); }

static void event_btn_volume(lv_event_t *e)
{
    if (lv_obj_has_flag(slider_volume, LV_OBJ_FLAG_HIDDEN))
        lv_obj_clear_flag(slider_volume, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(slider_volume, LV_OBJ_FLAG_HIDDEN);
    reset_vol_hide();
}

static void event_slider_volume(lv_event_t *e)
{
    int val = lv_slider_get_value(slider_volume);
    Volume_adjustment((uint8_t)val);
    if (val == 0) lv_label_set_text(label_volume_icon, " ");
    else if (val < 50) lv_label_set_text(label_volume_icon, LV_SYMBOL_VOLUME_MID);
    else lv_label_set_text(label_volume_icon, LV_SYMBOL_VOLUME_MAX);
    reset_vol_hide();
}

static void event_list_item(lv_event_t *e)
{
    uint16_t idx = (uint16_t)(intptr_t)lv_event_get_user_data(e);
    start_play(idx);
    lv_scr_load(screen_play);
}

/**********************
 *   UI CREATION
 **********************/

static lv_obj_t *create_ctrl_btn(lv_obj_t *parent, const char *symbol, const lv_font_t *font, bool highlight, uint16_t size)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    if (highlight) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_shadow_color(btn, lv_color_hex(0xe94560), 0);
        lv_obj_set_style_shadow_opa(btn, LV_OPA_40, 0);
        lv_obj_set_style_shadow_width(btn, 12, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_center(label);
    return btn;
}

static void create_play_screen(void)
{
    screen_play = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_play, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(screen_play, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_play, 0, 0);
    lv_obj_set_style_border_width(screen_play, 0, 0);
    lv_obj_clear_flag(screen_play, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_exit = lv_btn_create(screen_play);
    lv_obj_set_size(btn_exit, 64, 44);
    lv_obj_set_pos(btn_exit, 66, 39);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0x243B55), 0);
    lv_obj_set_style_radius(btn_exit, 14, 0);
    lv_obj_add_event_cb(btn_exit, event_btn_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *exit_label = lv_label_create(btn_exit);
    lv_label_set_text(exit_label, "返回");
    lv_obj_set_style_text_font(exit_label, &font_music_cjk, 0);
    lv_obj_center(exit_label);

    /* 歌曲名 (顶部居中, 圆内安全区) */
    label_title = lv_label_create(screen_play);
    lv_label_set_text(label_title, "");
    lv_obj_set_style_text_color(label_title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_title, &font_music_cjk, 0);
    lv_obj_set_width(label_title, 240);
    lv_obj_set_style_text_align(label_title,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_long_mode(label_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(label_title, LV_ALIGN_TOP_MID, 0, 88);

    lv_obj_t *quality = lv_label_create(screen_play);quality_label=quality;
    lv_label_set_text(quality, "SD 卡音乐");
    lv_obj_set_style_text_color(quality, lv_color_hex(0x8A9BB0), 0);
    lv_obj_set_style_text_font(quality, &font_music_cjk, 0);
    lv_obj_align(quality, LV_ALIGN_TOP_MID, 30, 53);

    /* 唱片 (居中偏上) */
    lv_obj_t *disc_area = lv_obj_create(screen_play);
    lv_obj_set_size(disc_area, DISC_SIZE + 16, DISC_SIZE + 16);
    lv_obj_align(disc_area, LV_ALIGN_TOP_MID, 0, 107);
    lv_obj_set_style_bg_opa(disc_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(disc_area, 0, 0);
    lv_obj_set_style_pad_all(disc_area, 0, 0);
    lv_obj_clear_flag(disc_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *disc = lv_obj_create(disc_area);
    lv_obj_set_size(disc, DISC_SIZE, DISC_SIZE);
    lv_obj_center(disc);
    lv_obj_set_style_bg_color(disc, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_border_width(disc, 3, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_SCROLLABLE);

    disc_arc = lv_arc_create(disc_area);
    lv_obj_set_size(disc_arc, DISC_SIZE + 14, DISC_SIZE + 14);
    lv_obj_center(disc_arc);
    lv_obj_set_style_arc_opa(disc_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(disc_arc, lv_color_hex(0xe94560), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(disc_arc, 4, LV_PART_INDICATOR);
    lv_obj_clear_flag(disc_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_start_angle(disc_arc, 0);
    lv_arc_set_end_angle(disc_arc, 70);

    lv_obj_t *label_disc_icon = lv_label_create(disc);
    lv_label_set_text(label_disc_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(label_disc_icon, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(label_disc_icon, &lv_font_montserrat_22, 0);
    lv_obj_center(label_disc_icon);

    /* 均衡器 */
    lv_obj_t *eq_area = lv_obj_create(screen_play);
    int eq_total_w = EQ_BAR_CNT * EQ_BAR_W + (EQ_BAR_CNT - 1) * 5;
    lv_obj_set_size(eq_area, eq_total_w, 24);
    lv_obj_align(eq_area, LV_ALIGN_TOP_MID, 0, 221);
    lv_obj_set_style_bg_opa(eq_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eq_area, 0, 0);
    lv_obj_set_style_pad_all(eq_area, 0, 0);
    lv_obj_clear_flag(eq_area, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < EQ_BAR_CNT; i++) {
        eq_bars[i] = lv_obj_create(eq_area);
        lv_obj_set_size(eq_bars[i], EQ_BAR_W, 6);
        lv_obj_set_style_bg_color(eq_bars[i], lv_color_hex(0xe94560), 0);
        lv_obj_set_style_bg_opa(eq_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(eq_bars[i], 2, 0);
        lv_obj_set_style_border_width(eq_bars[i], 0, 0);
        lv_obj_align(eq_bars[i], LV_ALIGN_BOTTOM_LEFT, i * (EQ_BAR_W + 5), 0);
    }

    /* 时间 */
    label_time = lv_label_create(screen_play);
    lv_label_set_text(label_time, "0:00");
    lv_obj_set_style_text_color(label_time, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_time, &font_music_cjk, 0);
    lv_obj_align(label_time, LV_ALIGN_TOP_MID, 0, 247);

    /* 音量滑块 (默认隐藏) */
    slider_volume = lv_slider_create(screen_play);
    lv_obj_set_size(slider_volume, 128, 8);
    lv_obj_set_style_pad_all(slider_volume,4,LV_PART_KNOB);
    lv_obj_align(slider_volume, LV_ALIGN_TOP_MID, 0, 332);
    lv_slider_set_range(slider_volume, 0, Volume_MAX);
    lv_slider_set_value(slider_volume, Volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0x333355), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0xe94560), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider_volume, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_flag(slider_volume, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(slider_volume, event_slider_volume, LV_EVENT_VALUE_CHANGED, NULL);

    /* 底部控制栏 - 5按钮, 放大便于触摸 */
    lv_obj_t *ctrl_bar = lv_obj_create(screen_play);
    lv_obj_set_size(ctrl_bar, 216, 48);
    lv_obj_align(ctrl_bar, LV_ALIGN_TOP_MID, 0, 268);
    lv_obj_set_style_bg_opa(ctrl_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_bar, 0, 0);
    lv_obj_set_style_pad_all(ctrl_bar, 0, 0);
    lv_obj_clear_flag(ctrl_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ctrl_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *b_list = create_ctrl_btn(ctrl_bar, LV_SYMBOL_LIST, &lv_font_montserrat_18, false, 40);
    lv_obj_add_event_cb(b_list, event_btn_list, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_prev = create_ctrl_btn(ctrl_bar, LV_SYMBOL_PREV, &lv_font_montserrat_18, false, 40);
    lv_obj_add_event_cb(b_prev, event_btn_prev, LV_EVENT_CLICKED, NULL);

    btn_play = create_ctrl_btn(ctrl_bar, LV_SYMBOL_PLAY, &lv_font_montserrat_22, true, 48);
    label_play_icon = lv_obj_get_child(btn_play, 0);
    lv_obj_add_event_cb(btn_play, event_btn_play, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_next = create_ctrl_btn(ctrl_bar, LV_SYMBOL_NEXT, &lv_font_montserrat_18, false, 40);
    lv_obj_add_event_cb(b_next, event_btn_next, LV_EVENT_CLICKED, NULL);

    lv_obj_t *b_vol = create_ctrl_btn(ctrl_bar, LV_SYMBOL_VOLUME_MAX, &lv_font_montserrat_18, false, 40);
    label_volume_icon = lv_obj_get_child(b_vol, 0);
    lv_obj_add_event_cb(b_vol, event_btn_volume, LV_EVENT_CLICKED, NULL);
}

void Music_List_Page(int delta) {
    unsigned pages=(Music_Track_Count+TRACKS_PER_PAGE-1)/TRACKS_PER_PAGE;
    if(!pages) return;
    list_page=(list_page+pages+delta)%pages;
    refresh_list();
}
static void list_page_event(lv_event_t *e) { Music_List_Page((int)(intptr_t)lv_event_get_user_data(e)); }

static void create_list_screen(void)
{
    screen_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen_list, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(screen_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_list, 0, 0);
    lv_obj_set_style_border_width(screen_list, 0, 0);
    lv_obj_clear_flag(screen_list, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部栏 */
    lv_obj_t *top_bar = lv_obj_create(screen_list);
    lv_obj_set_size(top_bar, 224, 44);
    lv_obj_align(top_bar, LV_ALIGN_TOP_MID, 0, 39);
    lv_obj_set_style_bg_opa(top_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar, 0, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_back = lv_btn_create(top_bar);
    lv_obj_set_size(btn_back, 64, 44);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(btn_back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(btn_back, event_btn_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon_back = lv_label_create(btn_back);
    lv_label_set_text(icon_back, "返回");
    lv_obj_set_style_text_color(icon_back, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(icon_back, &font_music_cjk, 0);
    lv_obj_center(icon_back);

    lv_obj_t *label_title_list = lv_label_create(top_bar);
    lv_label_set_text(label_title_list, "播放列表");
    lv_obj_set_style_text_color(label_title_list, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(label_title_list, &font_music_cjk, 0);
    lv_obj_align(label_title_list, LV_ALIGN_CENTER, 22, 0);

    lv_obj_t *label_count = lv_label_create(top_bar);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d首", Music_Track_Count);
    lv_label_set_text(label_count, buf);
    lv_obj_set_style_text_color(label_count, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(label_count, &font_music_cjk, 0);
    lv_obj_align(label_count, LV_ALIGN_RIGHT_MID, 0, 0);

    /* 列表容器 - 启用滚动 */
    list_cont = lv_obj_create(screen_list);
    lv_obj_set_size(list_cont, 256, 194);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 2, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    for(int direction=-1;direction<=1;direction+=2) {
        lv_obj_t *b=lv_btn_create(screen_list);lv_obj_set_size(b,76,40);
        lv_obj_set_pos(b,direction<0?90:194,290);
        lv_obj_t *l=lv_label_create(b);lv_label_set_text(l,direction<0?"上一页":"下一页");
        lv_obj_set_style_text_font(l,&font_music_cjk,0);lv_obj_center(l);
        lv_obj_add_event_cb(b,list_page_event,LV_EVENT_CLICKED,(void*)(intptr_t)direction);
    }
    list_page_label=lv_label_create(screen_list);
    lv_obj_set_style_text_font(list_page_label,&font_music_cjk,0);
    lv_obj_align(list_page_label,LV_ALIGN_BOTTOM_MID,0,-8);
    refresh_list();
}

static void refresh_list(void)
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);

    unsigned pages=(Music_Track_Count+TRACKS_PER_PAGE-1)/TRACKS_PER_PAGE;
    if(list_page_label) lv_label_set_text_fmt(list_page_label,"%u / %u",list_page+1,pages?pages:1);
    lv_obj_scroll_to_y(list_cont,0,LV_ANIM_OFF);
    for (uint16_t i = list_page*TRACKS_PER_PAGE; i < Music_Track_Count && i < (list_page+1)*TRACKS_PER_PAGE; i++) {
        lv_obj_t *item = lv_btn_create(list_cont);
        lv_obj_set_size(item, 244, 44);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_border_color(item, lv_color_hex(0x2a2a4a), 0);
        lv_obj_set_style_pad_all(item, 0, 0);

        if (i == cur_track) {
            lv_obj_set_style_bg_color(item, lv_color_hex(0xe94560), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(item, LV_OPA_30, 0);
            lv_obj_set_style_border_color(item, lv_color_hex(0xe94560), 0);
        }

        lv_obj_t *label_idx = lv_label_create(item);
        char idx_buf[8];
        snprintf(idx_buf, sizeof(idx_buf), "%d", i + 1);
        lv_label_set_text(label_idx, idx_buf);
        lv_obj_set_style_text_color(label_idx,
            (i == cur_track) ? lv_color_hex(0xe94560) : lv_color_hex(0x666688), 0);
        lv_obj_set_style_text_font(label_idx, &lv_font_montserrat_12, 0);
        lv_obj_align(label_idx, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *label_name = lv_label_create(item);
        lv_label_set_text(label_name, disp_names[i]);
        lv_obj_set_style_text_color(label_name,
            (i == cur_track) ? lv_color_hex(0xffffff) : lv_color_hex(0xcccccc), 0);
        lv_obj_set_style_text_font(label_name, &font_music_cjk, 0);
        lv_obj_set_size(label_name, 166, 20);
        lv_label_set_long_mode(label_name, LV_LABEL_LONG_DOT);
        lv_obj_align(label_name, LV_ALIGN_LEFT_MID, 40, 0);

        if (i == cur_track && is_playing) {
            lv_obj_t *label_playing = lv_label_create(item);
            lv_label_set_text(label_playing, LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_color(label_playing, lv_color_hex(0xe94560), 0);
            lv_obj_set_style_text_font(label_playing, &font_music_cjk, 0);
            lv_obj_align(label_playing, LV_ALIGN_RIGHT_MID, -8, 0);
        }

        lv_obj_add_event_cb(item, event_list_item, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

}

void Music_Dump_Tracks(void) {
    for (unsigned i=0;i<Music_Track_Count;i++) printf("LAB track %u %s\n",i,sd_names[i]);
}
