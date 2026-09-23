#include "PCM5101.h"
#include "TCA9554PWR.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdatomic.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#include "dr_flac.h"

static const char *TAG = "AUDIO PCM5101";

static i2s_chan_handle_t i2s_tx_chan;

uint8_t Volume = 20;  /* 默认音量20% */
bool Music_Next_Flag = 0;
uint32_t Audio_Bytes_Written = 0;

/* 文件类型 */
typedef enum {
    AUDIO_TYPE_MP3 = 0,
    AUDIO_TYPE_FLAC,
} audio_type_t;

static audio_type_t g_audio_type = AUDIO_TYPE_MP3;

/* MP3 解码器状态 */
EXT_RAM_BSS_ATTR static mp3dec_t g_dec;
static bool g_dec_inited = false;

/* FLAC 解码器状态 */
static drflac *g_flac = NULL;

static FILE *g_file = NULL;
static TaskHandle_t g_task = NULL;
static volatile bool g_running = false;
static volatile bool g_paused = false;
static volatile bool g_stop_req = false;

/* 待播放文件路径（由 Play_Music 设置，audio 任务打开） */
static char g_pending_path[256];
typedef struct { char path[256]; audio_type_t type; } play_request_t;
static QueueHandle_t play_requests;
static audio_type_t g_pending_type = AUDIO_TYPE_MP3;

/* MP3 静态缓冲区 */
EXT_RAM_BSS_ATTR static uint8_t g_input_buf[16384];
EXT_RAM_BSS_ATTR static int16_t g_pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
static int g_input_len = 0;
static int g_input_offset = 0;

/* FLAC PCM 缓冲区 */
#define FLAC_PCM_FRAMES 2048
EXT_RAM_BSS_ATTR static int16_t g_flac_pcm_buf[FLAC_PCM_FRAMES * 2];

static void close_current(void)
{
    if (g_flac) {
        drflac_close(g_flac);
        g_flac = NULL;
    }
    if (g_file && g_file != (FILE*)1) {
        fclose(g_file);
        g_file = NULL;
    }
    g_file = NULL;
    g_input_len = 0;
    g_input_offset = 0;
}

static uint32_t g_current_sr = 48000;
static volatile bool tone_requested;
static atomic_bool video_request,video_ack;
static unsigned video_channels;
bool Audio_Ready(void) { return g_running && g_task; }
void Audio_Test_Tone(void) { tone_requested = true; }

static void audio_set_sample_rate(uint32_t sr)
{
    if (sr == g_current_sr || sr == 0) return;
    i2s_channel_disable(i2s_tx_chan);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sr);
    i2s_channel_reconfig_std_clock(i2s_tx_chan, &clk_cfg);
    i2s_channel_enable(i2s_tx_chan);
    g_current_sr = sr;
    ESP_LOGI(TAG, "I2S sample rate changed to %lu", (unsigned long)sr);
}

bool Audio_Video_Begin(unsigned rate,unsigned channels) {
    if(!Audio_Ready() || channels<1 || channels>2 || rate<8000 || rate>48000)return false;
    g_paused=true;Music_Next_Flag=false;
    atomic_store(&video_request,true);
    for(unsigned i=0;i<200 && !atomic_load(&video_ack);i++)vTaskDelay(1);
    if(!atomic_load(&video_ack)) {atomic_store(&video_request,false);return false;}
    video_channels=channels;audio_set_sample_rate(rate);return true;
}
bool Audio_Video_PCM(const int16_t *samples,size_t count) {
    if(!atomic_load(&video_ack) || !video_channels || count%video_channels)return false;
    while(count) {
        size_t frames=count/video_channels;if(frames>FLAC_PCM_FRAMES)frames=FLAC_PCM_FRAMES;
        for(size_t i=0;i<frames;i++) {
            g_flac_pcm_buf[i*2]=(int32_t)samples[i*video_channels]*Volume/100;
            g_flac_pcm_buf[i*2+1]=(int32_t)samples[i*video_channels+(video_channels==2)]*Volume/100;
        }
        size_t written=0;
        if(i2s_channel_write(i2s_tx_chan,g_flac_pcm_buf,frames*4,&written,250)!=ESP_OK || written!=frames*4)return false;
        Audio_Bytes_Written+=written;samples+=frames*video_channels;count-=frames*video_channels;
    }
    return true;
}
void Audio_Video_End(void) {
    atomic_store(&video_request,false);
    /* Do not hand the buffers back to the media task until the owner acknowledges. */
    while(atomic_load(&video_ack))vTaskDelay(1);
}

/* FLAC PSRAM 分配回调 */
static void* flac_malloc(size_t size, void* pUserData)
{
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(size); /* 回退到内部RAM */
    return p;
}
static void flac_free(void* p, void* pUserData)
{
    free(p);
}
static drflac_allocation_callbacks g_flac_alloc = {
    .pUserData = NULL,
    .onMalloc = flac_malloc,
    .onRealloc = NULL,
    .onFree = flac_free,
};

static bool open_pending_file(void)
{
    close_current();

    if (g_pending_type == AUDIO_TYPE_FLAC) {
        /* 先检查文件是否存在 */
        FILE *fp_check = fopen(g_pending_path, "rb");
        if (!fp_check) {
            ESP_LOGE(TAG, "FLAC file not found: %s", g_pending_path);
            return false;
        }
        fseek(fp_check, 0, SEEK_END);
        long fsize = ftell(fp_check);
        fclose(fp_check);
        ESP_LOGI(TAG, "FLAC file size: %ld bytes", fsize);
        ESP_LOGI(TAG, "Free heap: %d, free PSRAM: %d",
                 (int)esp_get_free_heap_size(),
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        /* 读取文件头确认格式 */
        unsigned char hdr[8];
        FILE *fp_h = fopen(g_pending_path, "rb");
        if (fp_h) {
            size_t nr = fread(hdr, 1, 8, fp_h);
            fclose(fp_h);
            ESP_LOGI(TAG, "FLAC header: %02X %02X %02X %02X %02X %02X %02X %02X (read %d)",
                     hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7], (int)nr);
        }

        g_flac = drflac_open_file(g_pending_path, &g_flac_alloc);
        if (!g_flac) {
            ESP_LOGE(TAG, "drflac_open_file failed: %s", g_pending_path);
            return false;
        }
        if(g_flac->channels>2) {
            ESP_LOGE(TAG,"FLAC supports mono/stereo only");
            drflac_close(g_flac);g_flac=NULL;return false;
        }
        ESP_LOGI(TAG, "FLAC: %dHz, %dch, %llu frames",
                 g_flac->sampleRate, g_flac->channels,
                 (unsigned long long)g_flac->totalPCMFrameCount);
        audio_set_sample_rate(g_flac->sampleRate);
        g_file = (FILE*)1; /* 标记有文件在播放 */
        g_audio_type = AUDIO_TYPE_FLAC;
    } else {
        g_file = Open_File(g_pending_path);
        if (!g_file) {
            ESP_LOGE(TAG, "Failed to open: %s", g_pending_path);
            return false;
        }
        /* 找MP3帧起始 */
        unsigned char buf[4];
        fseek(g_file, 0, SEEK_SET);
        long mp3_start = -1;
        if (fread(buf, 1, 3, g_file) == 3 && memcmp(buf, "ID3", 3) == 0) {
            fseek(g_file, 3, SEEK_CUR);
            unsigned char sz[4];
            if (fread(sz, 1, 4, g_file) == 4) {
                int id3_size = ((int)sz[0]<<21)|((int)sz[1]<<14)|((int)sz[2]<<7)|sz[3];
                long pos = ftell(g_file) + id3_size;
                fseek(g_file, pos, SEEK_SET);
                if (fread(buf, 1, 4, g_file) == 4 && buf[0]==0xFF && (buf[1]&0xE0)==0xE0) {
                    mp3_start = pos;
                } else {
                    fseek(g_file, pos, SEEK_SET);
                }
            }
        }
        if (mp3_start < 0) {
            fseek(g_file, 0, SEEK_SET);
            for (int i = 0; i < 131072; i++) {
                long pos = ftell(g_file);
                if (fread(buf, 1, 4, g_file) != 4) break;
                if (buf[0]==0xFF && (buf[1]&0xE0)==0xE0) {
                    int br=(buf[2]>>4)&0xF, sr=(buf[2]>>2)&3;
                    if (br!=0 && br!=15 && sr!=3) { mp3_start = pos; break; }
                }
                fseek(g_file, -3, SEEK_CUR);
            }
        }
        if (mp3_start < 0) {
            ESP_LOGE(TAG, "No valid MP3 frame: %s", g_pending_path);
            fclose(g_file);
            g_file = NULL;
            return false;
        }
        fseek(g_file, mp3_start, SEEK_SET);
        mp3dec_init(&g_dec);
        g_audio_type = AUDIO_TYPE_MP3;
        ESP_LOGI(TAG, "Playing MP3: %s (at %ld)", g_pending_path, mp3_start);
    }

    Audio_Bytes_Written = 0;
    return true;
}

static void player_task(void *arg) {
    while (g_running) {
        if(atomic_load(&video_request)) {
            atomic_store(&video_ack,true);
            while(atomic_load(&video_request))vTaskDelay(1);
            atomic_store(&video_ack,false);
            continue;
        }
        if (tone_requested) {
            tone_requested=false;
            uint32_t saved_rate=g_current_sr;
            audio_set_sample_rate(16000);
            for (int block=0; block<32; block++) {
                for (int i=0;i<256;i++) {
                    int16_t v=((block*256+i)%32 < 16) ? 1000 : -1000;
                    g_flac_pcm_buf[2*i]=g_flac_pcm_buf[2*i+1]=v;
                }
                size_t written=0;
                esp_err_t err=i2s_channel_write(i2s_tx_chan,g_flac_pcm_buf,1024,&written,1000);
                if(err!=ESP_OK) { ESP_LOGE(TAG,"Tone write: %s",esp_err_to_name(err)); break; }
            }
            audio_set_sample_rate(saved_rate);
        }
        /* 处理新文件请求（在audio任务中打开，避免main任务栈溢出） */
        play_request_t request;
        if (xQueueReceive(play_requests,&request,0)==pdTRUE) {
            memcpy(g_pending_path,request.path,sizeof(g_pending_path));
            g_pending_type=request.type;
            g_paused = true;
            if (open_pending_file()) {
                g_paused = false;
                g_stop_req = false;
            }
            continue;
        }

        if (g_paused || g_stop_req || !g_file) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (g_audio_type == AUDIO_TYPE_MP3) {
            mp3dec_frame_info_t frame_info;

            if (g_input_len < 4096) {
                memmove(g_input_buf, g_input_buf + g_input_offset, g_input_len);
                g_input_offset = 0;
                size_t n = fread(g_input_buf + g_input_len, 1, sizeof(g_input_buf) - g_input_len, g_file);
                g_input_len += n;
            }

            int samples = mp3dec_decode_frame(&g_dec, g_input_buf + g_input_offset, g_input_len, g_pcm_buf, &frame_info);

            if (frame_info.frame_bytes > 0) {
                g_input_offset += frame_info.frame_bytes;
                g_input_len -= frame_info.frame_bytes;
            } else {
                g_input_offset++;
                g_input_len--;
                if (g_input_len <= 0) { g_input_len = 0; g_input_offset = 0; }
                continue;
            }

            if (samples > 0) {
                audio_set_sample_rate(frame_info.hz);
                if (frame_info.channels == 1) {
                    for (int i=samples-1;i>=0;i--) g_pcm_buf[2*i]=g_pcm_buf[2*i+1]=g_pcm_buf[i];
                    frame_info.channels=2;
                }
                float vol_factor = Volume / 100.0f;
                if (Volume < 100) {
                    for (int i = 0; i < samples * frame_info.channels; i++) {
                        g_pcm_buf[i] = (int16_t)(g_pcm_buf[i] * vol_factor);
                    }
                }
                size_t bytes = samples * frame_info.channels * sizeof(int16_t);
                size_t bytes_written = 0;
                i2s_channel_write(i2s_tx_chan, g_pcm_buf, bytes, &bytes_written, portMAX_DELAY);
                Audio_Bytes_Written += bytes_written;
            }

            if (g_input_len == 0 && feof(g_file)) {
                ESP_LOGI(TAG, "MP3 playback finished");
                Music_Next_Flag = 1;
                g_paused = true;
            }

        } else if (g_audio_type == AUDIO_TYPE_FLAC) {
            if (!g_flac) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

            drflac_uint64 read_count = drflac_read_pcm_frames_s16(g_flac, FLAC_PCM_FRAMES, g_flac_pcm_buf);

            if (read_count == 0) {
                ESP_LOGI(TAG, "FLAC playback finished");
                Music_Next_Flag = 1;
                g_paused = true;
                continue;
            }

            int channels = g_flac->channels;
            int total_samples = read_count * channels;

            float vol_factor = Volume / 100.0f;
            if (Volume < 100) {
                for (int i = 0; i < total_samples; i++) {
                    g_flac_pcm_buf[i] = (int16_t)(g_flac_pcm_buf[i] * vol_factor);
                }
            }

            /* 单声道转立体声 */
            if (channels == 1) {
                for (drflac_uint64 i = read_count; i > 0; i--) {
                    g_flac_pcm_buf[(i-1)*2] = g_flac_pcm_buf[i-1];
                    g_flac_pcm_buf[(i-1)*2+1] = g_flac_pcm_buf[i-1];
                }
                total_samples = read_count * 2;
            }

            size_t bytes = total_samples * sizeof(int16_t);
            size_t bytes_written = 0;
            i2s_channel_write(i2s_tx_chan, g_flac_pcm_buf, bytes, &bytes_written, portMAX_DELAY);
            Audio_Bytes_Written += bytes_written;
        }
    }

    vTaskDelete(NULL);
}

static esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config, i2s_chan_handle_t *tx_channel, i2s_chan_handle_t *rx_channel) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx_channel, rx_channel));
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = (i2s_config != NULL) ? i2s_config : &std_cfg_default;
    if (tx_channel) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(*tx_channel, p_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(*tx_channel));
    }
    if (rx_channel) {
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(*rx_channel, p_i2s_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(*rx_channel));
    }
    return ESP_OK;
}

void Audio_Init(void)
{
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = BSP_I2S_GPIO_CFG,
    };
    esp_err_t ret = bsp_audio_init(&std_cfg, &i2s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio: %s", esp_err_to_name(ret));
        return;
    }

    Set_EXIO(TCA9554_EXIO6, true);
    Set_EXIO(TCA9554_EXIO7, true);
    Set_EXIO(TCA9554_EXIO8, true);

    mp3dec_init(&g_dec);
    g_dec_inited = true;
    g_paused = true;

    play_requests=xQueueCreate(1,sizeof(play_request_t));
    if(!play_requests) {ESP_LOGE(TAG,"Audio request allocation failed");return;}
    /* The pinned task may start on core 1 before creation returns. */
    g_running = true;
    if(xTaskCreatePinnedToCore(player_task, "audio_player", 24576, NULL, 5, &g_task, 1) != pdPASS) g_running=false;
    if (!g_running) ESP_LOGE(TAG, "Audio task allocation failed");

    ESP_LOGI(TAG, "Audio init OK (mp3+flac), volume=%d", Volume);
}

bool Play_Music(const char* directory, const char* fileName)
{
    if (!g_running || !g_task) return false;
    char filePath[256];
    if (strcmp(directory, "/") == 0) {
        snprintf(filePath, sizeof(filePath), "%s%s", directory, fileName);
    } else {
        snprintf(filePath, sizeof(filePath), "%s/%s", directory, fileName);
    }

    ESP_LOGI(TAG, "Play_Music request: %s", filePath);

    play_request_t request={0};
    const char *dot=strrchr(fileName,'.');
    request.type=dot && !strcasecmp(dot,".flac") ? AUDIO_TYPE_FLAC : AUDIO_TYPE_MP3;
    if(strlen(directory)+strlen(fileName)+2>sizeof(request.path)) return false;
    snprintf(request.path,sizeof(request.path),"%s",filePath);
    Music_Next_Flag=false;
    return xQueueOverwrite(play_requests,&request)==pdTRUE;

}

void Music_resume(void)
{
    g_paused = false;
    g_stop_req = false;
}

void Music_pause(void)
{
    g_paused = true;
}

void Volume_adjustment(uint8_t Vol) {
    if(Vol > Volume_MAX)
        printf("Audio : The volume value is incorrect. Please enter 0 to 100\r\n");
    else
        Volume = Vol;
}
