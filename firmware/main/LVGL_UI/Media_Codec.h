#pragma once
#include "lvgl.h"
#include <stdio.h>
#include <stdbool.h>
typedef bool (*media_cancel_fn)(void *);
typedef struct {
    FILE *file;
    uint32_t movi_start,movi_end,next,frame_us,frames;
    unsigned width,height,video_stream,audio_stream,audio_rate,audio_channels;
    bool pcm;
} media_avi_t;
bool Media_Decode_JPEG(FILE *file,uint32_t bytes,lv_color_t *pixels,unsigned *width,unsigned *height,unsigned orientation,media_cancel_fn cancel,void *ctx,char *error,size_t error_size);
bool Media_Decode_Photo(const char *path,lv_color_t *pixels,unsigned *width,unsigned *height,media_cancel_fn cancel,void *ctx,char *error,size_t error_size);
bool Media_Photo_Cached(const char *path,lv_color_t *pixels,unsigned *width,unsigned *height,media_cancel_fn cancel,void *ctx,char *error,size_t error_size,unsigned *cache_state);
bool Media_AVI_Open(FILE *file,media_avi_t *avi,char *error,size_t error_size);
/* 1=video JPEG, 2=PCM, 0=end, -1=invalid; caller seeks/reads returned payload. */
int Media_AVI_Next(media_avi_t *avi,uint32_t *offset,uint32_t *bytes);
bool Media_PNG_Decode(const uint8_t *data,size_t size,lv_color_t *pixels,unsigned *w,unsigned *h,char *error,size_t error_size);
