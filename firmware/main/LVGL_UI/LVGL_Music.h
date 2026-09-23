#pragma once

#include "lvgl.h"
#include "SD_MMC.h"
#include "PCM5101.h"

#define MUSIC_MAX_TRACKS    1024
#define MUSIC_NAME_LEN      256
#define MUSIC_SD_PATH       "/sdcard"

/* 歌曲数量 */
extern uint16_t Music_Track_Count;

/* 音乐播放器入口：扫描SD卡、创建UI、等待用户点击播放 */
void Music_Player_Init(void);

/* 供外部调用的控制接口 */
void Music_Play_Track(uint16_t index);
void Music_Play_Pause_Toggle(void);
void Music_Resume(void);
void Music_Next(void);
void Music_Prev(void);
void Music_Set_Volume(uint8_t vol);

void Music_Show(void);
void Music_Set_Back_Callback(void (*callback)(void));

void Music_Show_List(void);

void Music_Dump_Tracks(void);

void Music_Pause(void);
void Music_List_Page(int delta);
