#pragma once
#include "lvgl.h"
void Touch_Calibration_Init(void);
void Touch_Calibration_Start(void);
void Touch_Calibration_Cancel(void);
bool Touch_Calibration_Sample(bool pressed, int x, int y);
void Touch_Map(int x, int y, lv_point_t *logical);
