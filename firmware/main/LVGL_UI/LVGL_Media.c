#include "LVGL_Media.h"
#include "Media_Codec.h"
#include "LVGL_Music.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "esp_rom_crc.h"
#include "driver/usb_serial_jtag.h"

#define MEDIA_MAX 512
#define PATH_MAX_MEDIA 512
#define PER_PAGE 8
typedef enum {PHOTO_JPEG,PHOTO_PNG,PHOTO_BMP,VIDEO_AVI,VIDEO_MJPEG,VIDEO_OTHER} media_kind_t;
typedef struct {char path[PATH_MAX_MEDIA];media_kind_t kind;} entry_t;
EXT_RAM_BSS_ATTR static entry_t entries[MEDIA_MAX];
EXT_RAM_BSS_ATTR static char directories[128][PATH_MAX_MEDIA];
static unsigned entry_count,photo_count,video_count;
static atomic_bool scanned,paused;
static atomic_uint generation;
static atomic_uint cache_done,cache_failed,cache_total;
typedef struct {unsigned generation,index;bool scan,prepare;} command_t;
typedef enum {EVENT_SCAN,EVENT_FRAME,EVENT_ERROR,EVENT_END,EVENT_CACHE} event_type_t;
typedef struct {event_type_t type;unsigned generation,buffer,w,h,frame,frames,ms;bool audio;char text[160];} event_t;
static QueueHandle_t commands,events;
static SemaphoreHandle_t buffer_free[2];
static lv_color_t *pixels[2];
static lv_img_dsc_t images[2];
static lv_obj_t *screen,*list,*heading,*back,*pager,*page_label,*info,*image,*controls,*filename,*play,*time_label;
static lv_timer_t *timer;
static void (*go_home)(void);
static bool visible,video_mode,viewing,controls_hidden,ended,ready;
static int front=-1,current=-1;
static unsigned list_page,displayed_frames,displayed_ms,total_ms;
static uint32_t last_action;
static void draw_list(void);
extern const lv_font_t font_music_cjk;

static bool is_video(media_kind_t kind) {return kind>=VIDEO_AVI;}
static bool cancelled(void *ctx) {return atomic_load(&generation)!=(unsigned)(uintptr_t)ctx;}
static void send_event(event_t *e) {
    while(!cancelled((void *)(uintptr_t)e->generation) && xQueueSend(events,e,pdMS_TO_TICKS(20))!=pdTRUE) {}
}
static int compare_entries(const void *a,const void *b) {return strcasecmp(((const entry_t *)a)->path,((const entry_t *)b)->path);}
static void scan_files(unsigned gen) {
    entry_count=photo_count=video_count=0;unsigned head=0,tail=1,skipped=0;
    strcpy(directories[0],"/sdcard");
    while(head<tail && !cancelled((void *)(uintptr_t)gen)) {
        DIR *d=opendir(directories[head]);
        if(!d) {head++;skipped++;continue;}
        struct dirent *item;
        while((item=readdir(d)) && !cancelled((void *)(uintptr_t)gen)) {
            if(!strcmp(item->d_name,".") || !strcmp(item->d_name,"..") || !strcmp(item->d_name,".lab_media_cache") || !strcmp(item->d_name,"System Volume Information"))continue;
            char path[PATH_MAX_MEDIA];int n=snprintf(path,sizeof(path),"%s/%s",directories[head],item->d_name);
            if(n<0 || n>=sizeof(path)) {skipped++;continue;}
            struct stat st;if(stat(path,&st))continue;
            if(S_ISDIR(st.st_mode)) {if(tail<128)strcpy(directories[tail++],path);else skipped++;continue;}
            const char *ext=strrchr(item->d_name,'.');if(!ext)continue;
            media_kind_t kind;
            if(!strcasecmp(ext,".jpg") || !strcasecmp(ext,".jpeg"))kind=PHOTO_JPEG;
            else if(!strcasecmp(ext,".png"))kind=PHOTO_PNG;
            else if(!strcasecmp(ext,".bmp"))kind=PHOTO_BMP;
            else if(!strcasecmp(ext,".avi"))kind=VIDEO_AVI;
            else if(!strcasecmp(ext,".mjpg") || !strcasecmp(ext,".mjpeg"))kind=VIDEO_MJPEG;
            else if(!strcasecmp(ext,".mp4") || !strcasecmp(ext,".mov") || !strcasecmp(ext,".mkv") || !strcasecmp(ext,".webm") || !strcasecmp(ext,".3gp"))kind=VIDEO_OTHER;
            else continue;
            if(entry_count>=MEDIA_MAX) {skipped++;continue;}
            strcpy(entries[entry_count].path,path);entries[entry_count++].kind=kind;
            if(is_video(kind))video_count++;else photo_count++;
        }
        closedir(d);head++;vTaskDelay(1);
    }
    if(cancelled((void *)(uintptr_t)gen))return;
    qsort(entries,entry_count,sizeof(entries[0]),compare_entries);
    atomic_store(&scanned,true);
    event_t e={.type=EVENT_SCAN,.generation=gen};
    snprintf(e.text,sizeof(e.text),"照片 %u / 视频 %u%s",photo_count,video_count,skipped?" / 部分目录或文件未收录":"");send_event(&e);
    printf("LAB media scan photos=%u videos=%u skipped=%u\n",photo_count,video_count,skipped);
}
static bool acquire_buffer(unsigned n,unsigned gen) {
    while(!cancelled((void *)(uintptr_t)gen))if(xSemaphoreTake(buffer_free[n],pdMS_TO_TICKS(20))==pdTRUE)return true;
    return false;
}
static bool publish_frame(unsigned gen,unsigned buffer,unsigned w,unsigned h,unsigned frame,unsigned frames,unsigned ms,bool audio) {
    event_t e={.type=EVENT_FRAME,.generation=gen,.buffer=buffer,.w=w,.h=h,.frame=frame,.frames=frames,.ms=ms,.audio=audio};
    while(!cancelled((void *)(uintptr_t)gen))if(xQueueSend(events,&e,pdMS_TO_TICKS(20))==pdTRUE)return true;
    xSemaphoreGive(buffer_free[buffer]);return false;
}
static bool wait_play(unsigned gen,int64_t *deadline) {
    while(atomic_load(&paused) && !cancelled((void *)(uintptr_t)gen)) {
        int64_t before=esp_timer_get_time();vTaskDelay(pdMS_TO_TICKS(20));*deadline+=esp_timer_get_time()-before;
    }
    return !cancelled((void *)(uintptr_t)gen);
}
/* Raw MJPEG framing: skip length-coded marker payloads (including EXIF thumbnails)
 * and entropy stuffing, instead of treating every FF D9 byte pair as frame end. */
static bool next_mjpeg(FILE *f,uint32_t *start,uint32_t *bytes) {
    int previous=0,c;
    while((c=fgetc(f))!=EOF) {if(previous==0xff && c==0xd8)break;previous=c;}
    if(c==EOF)return false;
    long begin=ftell(f)-2;bool entropy=false;
    for(;;) {
        c=fgetc(f);if(c==EOF)return false;
        if(c!=0xff) {if(entropy)continue;return false;}
        do{c=fgetc(f);}while(c==0xff);
        if(c==EOF)return false;
        if(entropy && (c==0 || (c>=0xd0 && c<=0xd7)))continue;
        if(c==0xd9) {*start=begin;*bytes=ftell(f)-begin;return true;}
        int hi=fgetc(f),lo=fgetc(f);if(hi==EOF || lo==EOF)return false;
        int length=(hi<<8)|lo;if(length<2 || fseek(f,length-2,SEEK_CUR))return false;
        entropy=c==0xda;
        if(ftell(f)-begin>4194304)return false;
    }
}
static void play_file(command_t command) {
    unsigned gen=command.generation,index=command.index,buffer=0,w=0,h=0;
    char error[160]="读取失败";
    if(index>=entry_count)return;
    entry_t selected=entries[index];entry_t *entry=&selected;
    if(entry->kind==VIDEO_OTHER) {
        const char *name=strrchr(entry->path,'/');name=name?name+1:entry->path;
        char copy[PATH_MAX_MEDIA];struct stat st;
        int n=snprintf(copy,sizeof(copy),"/sdcard/device_lab_media/%s.screen.avi",name);
        if(n>0 && n<sizeof(copy) && !stat(copy,&st) && st.st_size>256) {strcpy(entry->path,copy);entry->kind=VIDEO_AVI;}
    }
    if(!is_video(entry->kind)) {
        if(!acquire_buffer(buffer,gen))return;
        unsigned hit=0;int64_t start=esp_timer_get_time();
        bool ok=Media_Photo_Cached(entry->path,pixels[buffer],&w,&h,cancelled,(void *)(uintptr_t)gen,error,sizeof(error),&hit);
        printf("LAB media photo index=%u ok=%d cached=%d decode_ms=%lld dimensions=%ux%u\n",index,ok,hit,(long long)((esp_timer_get_time()-start)/1000),w,h);
        if(ok) {publish_frame(gen,buffer,w,h,1,1,0,false);return;}
        xSemaphoreGive(buffer_free[buffer]);
    } else if(entry->kind==VIDEO_OTHER) {
        snprintf(error,sizeof(error),"该视频需要转换为 MJPEG AVI\n原文件不会修改");
    } else {
        FILE *f=fopen(entry->path,"rb");
        if(!f)snprintf(error,sizeof(error),"无法打开视频文件");
        else {
            char *file_buffer=heap_caps_malloc(16384,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(file_buffer)setvbuf(f,file_buffer,_IOFBF,16384);
            media_avi_t avi={0};bool raw=entry->kind==VIDEO_MJPEG,audio=false,ok=raw || Media_AVI_Open(f,&avi,error,sizeof(error));
            if(ok && avi.pcm)audio=Audio_Video_Begin(avi.audio_rate,avi.audio_channels);
            uint32_t frame=0,frame_us=raw?83333:avi.frame_us;
            int64_t deadline=esp_timer_get_time();
            while(ok && wait_play(gen,&deadline)) {
                uint32_t offset=0,bytes=0;int kind;
                if(raw)kind=next_mjpeg(f,&offset,&bytes)?1:0;
                else kind=Media_AVI_Next(&avi,&offset,&bytes);
                if(kind==0) {
                    if(!frame) {snprintf(error,sizeof(error),"视频内没有可解码的画面");ok=false;break;}
                    event_t e={.type=EVENT_END,.generation=gen};send_event(&e);break;
                }
                if(kind<0 || bytes>4194304 || fseek(f,offset,SEEK_SET)) {snprintf(error,sizeof(error),"视频文件损坏或帧过大");ok=false;break;}
                if(kind==2) {
                    if(audio) {
                        int16_t samples[512];
                        if(bytes%(avi.audio_channels*2)) {snprintf(error,sizeof(error),"视频音频数据损坏");ok=false;break;}
                        while(bytes && ok && wait_play(gen,&deadline)) {
                            size_t n=LV_MIN(bytes,sizeof(samples));
                            if(fread(samples,1,n,f)!=n || !Audio_Video_PCM(samples,n/2)) {snprintf(error,sizeof(error),"视频音频输出失败");ok=false;break;}
                            bytes-=n;
                        }
                    }
                    continue;
                }
                int64_t decode_start=esp_timer_get_time();
                if(!acquire_buffer(buffer,gen))break;
                int64_t acquired=esp_timer_get_time();
                ok=Media_Decode_JPEG(f,bytes,pixels[buffer],&w,&h,1,cancelled,(void *)(uintptr_t)gen,error,sizeof(error));
                if(frame==0 || frame%60==0)printf("LAB media video frame=%lu decode_ms=%lld wait_ms=%lld\n",(unsigned long)frame,(long long)((esp_timer_get_time()-acquired)/1000),(long long)((acquired-decode_start)/1000));
                if(raw)fseek(f,offset+bytes,SEEK_SET);
                if(!ok) {xSemaphoreGive(buffer_free[buffer]);break;}
                while(esp_timer_get_time()<deadline && wait_play(gen,&deadline))vTaskDelay(1);
                frame++;
                if(!publish_frame(gen,buffer,w,h,frame,raw?0:avi.frames,(uint64_t)frame*frame_us/1000,audio))break;
                buffer^=1;deadline+=frame_us;
                /* On slow media, present frames in order rather than accumulating a burst. */
                if(esp_timer_get_time()-deadline>500000)deadline=esp_timer_get_time();
            }
            if(audio)Audio_Video_End();
            fclose(f);free(file_buffer);
            if(ok || cancelled((void *)(uintptr_t)gen))return;
        }
    }
    if(!cancelled((void *)(uintptr_t)gen)) {event_t e={.type=EVENT_ERROR,.generation=gen};snprintf(e.text,sizeof(e.text),"%s",error);send_event(&e);}
}
static void prepare_photos(unsigned gen) {
    atomic_store(&cache_done,0);atomic_store(&cache_failed,0);atomic_store(&cache_total,photo_count);
    for(unsigned i=0;i<entry_count && !cancelled((void *)(uintptr_t)gen);i++) {
        if(is_video(entries[i].kind))continue;
        unsigned buffer=0;bool owned=false;
        while(!owned && !cancelled((void *)(uintptr_t)gen)) {
            if(xSemaphoreTake(buffer_free[0],0)==pdTRUE) {buffer=0;owned=true;}
            else if(xSemaphoreTake(buffer_free[1],0)==pdTRUE) {buffer=1;owned=true;}
            else vTaskDelay(1);
        }
        if(!owned)break;
        unsigned w=0,h=0,hit=0;char error[160]="缓存写入失败";int64_t start=esp_timer_get_time();
        bool ok=Media_Photo_Cached(entries[i].path,pixels[buffer],&w,&h,cancelled,(void *)(uintptr_t)gen,error,sizeof(error),&hit);
        xSemaphoreGive(buffer_free[buffer]);
        if(cancelled((void *)(uintptr_t)gen))break;
        ok=ok && hit!=0;
        if(ok)atomic_fetch_add(&cache_done,1);else atomic_fetch_add(&cache_failed,1);
        printf("LAB media cache index=%u ok=%d hit=%d ms=%lld%s%s\n",i,ok,hit,(long long)((esp_timer_get_time()-start)/1000),ok?"":" error=",ok?"":error);
        event_t e={.type=EVENT_CACHE,.generation=gen};send_event(&e);vTaskDelay(1);
    }
    printf("LAB media cache done=%u failed=%u total=%u\n",atomic_load(&cache_done),atomic_load(&cache_failed),atomic_load(&cache_total));
}
static void worker(void *arg) {
    command_t c;
    for(;;)if(xQueueReceive(commands,&c,portMAX_DELAY)==pdTRUE) {
        if(cancelled((void *)(uintptr_t)c.generation))continue;
        if(c.scan)scan_files(c.generation);else if(c.prepare)prepare_photos(c.generation);else play_file(c);
    }
}
static void release_front(void) {
    if(image)lv_obj_add_flag(image,LV_OBJ_FLAG_HIDDEN);
    if(front>=0) {xSemaphoreGive(buffer_free[front]);front=-1;}
}
static void cancel_work(void) {atomic_fetch_add(&generation,1);atomic_store(&paused,false);release_front();}
static void set_hidden(lv_obj_t *obj,bool hide) {if(hide)lv_obj_add_flag(obj,LV_OBJ_FLAG_HIDDEN);else lv_obj_clear_flag(obj,LV_OBJ_FLAG_HIDDEN);}
static void set_controls(bool hide) {
    controls_hidden=hide;set_hidden(back,hide);set_hidden(heading,hide);set_hidden(controls,hide);set_hidden(filename,hide);set_hidden(time_label,hide);
}
static lv_obj_t *text(lv_obj_t *parent,const char *value,int x,int y,int width) {
    lv_obj_t *o=lv_label_create(parent);lv_label_set_text(o,value);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,width,20);lv_obj_set_style_text_font(o,&font_music_cjk,0);lv_obj_set_style_text_color(o,lv_color_hex(0xE8F0FC),0);lv_label_set_long_mode(o,LV_LABEL_LONG_DOT);return o;
}
static lv_obj_t *button(lv_obj_t *parent,const char *value,int x,int y,int w,int h,lv_event_cb_t cb,void *data) {
    lv_obj_t *o=lv_btn_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);lv_obj_set_style_bg_color(o,lv_color_hex(0x172233),0);lv_obj_set_style_bg_opa(o,LV_OPA_80,0);lv_obj_set_style_radius(o,12,0);lv_obj_set_style_shadow_width(o,0,0);lv_obj_set_style_pad_all(o,0,0);
    lv_obj_t *l=text(o,value,0,0,w);lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(l);lv_obj_add_event_cb(o,cb,LV_EVENT_CLICKED,data);return o;
}
static void back_click(lv_event_t *e) {Media_Back();}
static void next_click(lv_event_t *e) {Media_Next((int)(intptr_t)lv_event_get_user_data(e));}
static void toggle_click(lv_event_t *e) {Media_Toggle();}
static void item_click(lv_event_t *e) {Media_Debug_Open((unsigned)(uintptr_t)lv_event_get_user_data(e));}
static void page_click(lv_event_t *e) {Media_Debug_List_Page((int)(intptr_t)lv_event_get_user_data(e));}
static void scan_click(lv_event_t *e) {Media_Rescan();}
static lv_point_t surface_start;
static bool surface_swiped;
static void surface_event(lv_event_t *e) {
    if(!viewing)return;
    lv_indev_t *indev=lv_indev_get_act();
    if(lv_event_get_code(e)==LV_EVENT_PRESSED) {
        lv_indev_get_point(indev,&surface_start);surface_swiped=false;
    } else if(lv_event_get_code(e)==LV_EVENT_RELEASED) {
        lv_point_t end;lv_indev_get_point(indev,&end);
        int dx=end.x-surface_start.x,dy=end.y-surface_start.y;
        /* Use distance rather than velocity, so slow finger swipes also work. */
        if(abs(dx)>=50 && abs(dx)>abs(dy)*3/2) {
            surface_swiped=true;Media_Next(dx<0?1:-1);
        } else if(dy>=65 && dy>abs(dx)*3/2) {
            surface_swiped=true;Media_Back();
        }
    } else if(lv_event_get_code(e)==LV_EVENT_CLICKED && !surface_swiped) {
        last_action=lv_tick_get();set_controls(!controls_hidden);
    }
}
static void draw_list(void) {
    viewing=false;set_hidden(list,false);set_hidden(pager,false);set_hidden(info,false);set_hidden(controls,true);set_hidden(filename,true);set_hidden(time_label,true);set_hidden(back,false);set_hidden(heading,false);
    lv_label_set_text(heading,video_mode?"视频播放":"照片查看");lv_obj_clean(list);lv_obj_scroll_to_y(list,0,LV_ANIM_OFF);
    lv_obj_set_pos(info,65,175);
    if(!ready) {lv_label_set_text(info,"媒体服务内存不足");return;}
    if(!atomic_load(&scanned)) {lv_label_set_text(info,"正在扫描 SD 卡…");return;}
    unsigned count=video_mode?video_count:photo_count,pages=(count+PER_PAGE-1)/PER_PAGE;
    if(list_page>=pages)list_page=pages?pages-1:0;
    lv_label_set_text_fmt(page_label,"%u / %u",list_page+1,pages?pages:1);
    if(!count) {
        lv_label_set_text(info,video_mode?"SD 卡未找到视频\n支持 MJPEG AVI / MJPEG":"SD 卡未找到照片\n支持 JPG / PNG / BMP");return;
    }
    lv_obj_set_pos(info,65,77);lv_label_set_text_fmt(info,"共 %u 个文件 / 点击查看",count);
    unsigned visible_index=0;
    for(unsigned i=0;i<entry_count;i++) {
        if(is_video(entries[i].kind)!=video_mode)continue;
        unsigned pos=visible_index++;if(pos<list_page*PER_PAGE || pos>=(list_page+1)*PER_PAGE)continue;
        const char *name=strrchr(entries[i].path,'/');name=name?name+1:entries[i].path;
        lv_obj_t *b=button(list,name,0,0,244,46,item_click,(void *)(uintptr_t)i);
        lv_obj_t *l=lv_obj_get_child(b,0);lv_obj_set_align(l,LV_ALIGN_TOP_LEFT);lv_obj_set_width(l,220);lv_obj_set_pos(l,12,3);
        static const char *types[]={"JPG","PNG","BMP","AVI","MJPEG","MP4 / MOV"};
        char detail[48];snprintf(detail,sizeof(detail),"%03u   %s",pos+1,types[entries[i].kind]);
        lv_obj_t *meta=text(b,detail,12,24,220);lv_obj_set_style_text_color(meta,lv_color_hex(0x8A9BB0),0);
    }
}
static void update(lv_timer_t *t) {
    event_t e;
    while(events && xQueueReceive(events,&e,0)==pdTRUE) {
        if(e.generation!=atomic_load(&generation) || !visible) {if(e.type==EVENT_FRAME)xSemaphoreGive(buffer_free[e.buffer]);continue;}
        if(e.type==EVENT_SCAN) {draw_list();puts(e.text);}
        else if(e.type==EVENT_CACHE) {if(!viewing)lv_label_set_text_fmt(info,"已加速 %u / %u",atomic_load(&cache_done),atomic_load(&cache_total));}
        else if(e.type==EVENT_FRAME) {
            int old=front;front=e.buffer;
            lv_img_cache_invalidate_src(&images[front]);lv_img_set_src(image,&images[front]);set_hidden(image,false);lv_obj_invalidate(image);
            if(old>=0)xSemaphoreGive(buffer_free[old]);
            displayed_frames=e.frame;displayed_ms=e.ms;
            total_ms=e.frames && e.frame?(uint64_t)e.ms*e.frames/e.frame:0;
            char value[80];snprintf(value,sizeof(value),"%u x %u%s",e.w,e.h,video_mode?(e.audio?" / 有声":" / 无声"):"");lv_label_set_text(heading,value);
            if(video_mode) {snprintf(value,sizeof(value),"%02u:%02u / %02u:%02u",e.ms/60000,(e.ms/1000)%60,total_ms/60000,(total_ms/1000)%60);lv_label_set_text(time_label,value);}
            set_hidden(info,true);
        } else if(e.type==EVENT_ERROR) {lv_label_set_text(info,e.text);set_hidden(info,false);lv_obj_move_foreground(info);set_controls(false);ended=true;atomic_store(&paused,true);lv_label_set_text(lv_obj_get_child(play,0),video_mode?"重试":"列表");printf("LAB media error %s\n",e.text);}
        else {ended=true;atomic_store(&paused,true);lv_label_set_text(lv_obj_get_child(play,0),"重播");set_controls(false);puts("LAB media ended");}
    }
    if(visible && viewing && front>=0 && !ended && !controls_hidden && lv_tick_elaps(last_action)>3000)set_controls(true);
}
void Media_Init(void (*home)(void)) {
    go_home=home;
    for(unsigned i=0;i<2;i++) {
        pixels[i]=heap_caps_malloc(360*360*sizeof(lv_color_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        buffer_free[i]=xSemaphoreCreateBinary();if(buffer_free[i])xSemaphoreGive(buffer_free[i]);
        images[i]=(lv_img_dsc_t){.header={.cf=LV_IMG_CF_TRUE_COLOR,.w=360,.h=360},.data_size=360*360*sizeof(lv_color_t),.data=(const uint8_t *)pixels[i]};
    }
    commands=xQueueCreate(1,sizeof(command_t));events=xQueueCreate(4,sizeof(event_t));
    ready=pixels[0] && pixels[1] && buffer_free[0] && buffer_free[1] && commands && events;
    if(ready)ready=xTaskCreatePinnedToCore(worker,"sd_media",8192,NULL,2,NULL,0)==pdPASS;
    screen=lv_obj_create(NULL);lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(screen,lv_color_hex(0x0B101A),0);lv_obj_set_style_text_font(screen,&font_music_cjk,0);
    image=lv_img_create(screen);lv_obj_set_pos(image,0,0);lv_obj_add_flag(image,LV_OBJ_FLAG_CLICKABLE);lv_obj_clear_flag(image,LV_OBJ_FLAG_GESTURE_BUBBLE|LV_OBJ_FLAG_SCROLLABLE);set_hidden(image,true);lv_obj_add_event_cb(image,surface_event,LV_EVENT_ALL,NULL);
    back=button(screen,"返回",66,39,64,44,back_click,NULL);
    heading=text(screen,"照片查看",140,52,155);
    list=lv_obj_create(screen);lv_obj_set_pos(list,52,94);lv_obj_set_size(list,256,182);lv_obj_set_style_bg_opa(list,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(list,0,0);lv_obj_set_style_pad_all(list,5,0);lv_obj_set_style_pad_row(list,6,0);lv_obj_set_flex_flow(list,LV_FLEX_FLOW_COLUMN);lv_obj_set_scroll_dir(list,LV_DIR_VER);
    info=text(screen,"",65,175,230);lv_label_set_long_mode(info,LV_LABEL_LONG_WRAP);lv_obj_set_height(info,LV_SIZE_CONTENT);lv_obj_set_style_text_align(info,LV_TEXT_ALIGN_CENTER,0);
    pager=lv_obj_create(screen);lv_obj_remove_style_all(pager);lv_obj_set_pos(pager,73,282);lv_obj_set_size(pager,214,58);
    button(pager,"上一页",0,0,66,32,page_click,(void *)(intptr_t)-1);button(pager,"扫描",74,0,66,32,scan_click,NULL);button(pager,"下一页",148,0,66,32,page_click,(void *)(intptr_t)1);page_label=text(pager,"1 / 1",60,38,100);lv_obj_set_style_text_align(page_label,LV_TEXT_ALIGN_CENTER,0);
    filename=text(screen,"",55,229,250);lv_obj_set_style_text_align(filename,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_bg_color(filename,lv_color_black(),0);lv_obj_set_style_bg_opa(filename,LV_OPA_60,0);
    time_label=text(screen,"",90,252,180);lv_obj_set_style_text_align(time_label,LV_TEXT_ALIGN_CENTER,0);lv_obj_set_style_bg_color(time_label,lv_color_black(),0);lv_obj_set_style_bg_opa(time_label,LV_OPA_60,0);
    controls=lv_obj_create(screen);lv_obj_remove_style_all(controls);lv_obj_set_pos(controls,82,278);lv_obj_set_size(controls,196,44);
    button(controls,LV_SYMBOL_PREV,0,0,52,44,next_click,(void *)(intptr_t)-1);play=button(controls,"暂停",60,0,76,44,toggle_click,NULL);button(controls,LV_SYMBOL_NEXT,144,0,52,44,next_click,(void *)(intptr_t)1);
    timer=lv_timer_create(update,20,NULL);draw_list();
}
void Media_Hide(void) {if(!visible)return;visible=false;cancel_work();viewing=false;}
void Media_Show(bool video) {
    Media_Hide();visible=true;video_mode=video;list_page=0;lv_scr_load(screen);draw_list();
    if(!atomic_load(&scanned) && ready)Media_Rescan();
}
void Media_Rescan(void) {
    if(!ready)return;
    cancel_work();atomic_store(&scanned,false);draw_list();
    command_t c={.generation=atomic_load(&generation),.scan=true};xQueueOverwrite(commands,&c);
}
void Media_Prepare(void) {
    if(!ready || !atomic_load(&scanned))return;
    cancel_work();if(visible)draw_list();
    command_t c={.generation=atomic_load(&generation),.prepare=true};xQueueOverwrite(commands,&c);
}
void Media_Back(void) {
    if(viewing) {cancel_work();draw_list();}else {Media_Hide();if(go_home)go_home();}
}
void Media_Debug_Open(unsigned index) {
    if(!ready || !atomic_load(&scanned) || index>=entry_count)return;
    if(!visible)visible=true;
    cancel_work();video_mode=is_video(entries[index].kind);viewing=true;current=index;ended=false;displayed_frames=displayed_ms=total_ms=0;last_action=lv_tick_get();
    if(video_mode)Music_Pause();
    lv_scr_load(screen);set_hidden(list,true);set_hidden(pager,true);set_controls(false);set_hidden(info,false);lv_obj_set_pos(info,65,175);lv_obj_move_foreground(info);lv_label_set_text(info,"正在读取…");
    lv_label_set_text(lv_obj_get_child(play,0),video_mode?"暂停":"列表");
    const char *name=strrchr(entries[index].path,'/');lv_label_set_text(filename,name?name+1:entries[index].path);lv_label_set_text(heading,video_mode?"视频播放":"照片查看");lv_label_set_text(time_label,video_mode?"00:00":"左右滑动切换");
    command_t c={.generation=atomic_load(&generation),.index=index};xQueueOverwrite(commands,&c);
    printf("LAB media open index=%u path=%s\n",index,entries[index].path);
}
void Media_Next(int delta) {
    if(!viewing || current<0 || !entry_count)return;
    int i=current;for(unsigned count=0;count<entry_count;count++) {i=(i+(delta<0?-1:1)+(int)entry_count)%entry_count;if(is_video(entries[i].kind)==video_mode) {Media_Debug_Open(i);break;}}
}
void Media_Toggle(void) {
    if(!viewing)return;
    if(!video_mode) {Media_Back();return;}
    if(ended) {Media_Debug_Open(current);return;}
    bool state=!atomic_load(&paused);atomic_store(&paused,state);last_action=lv_tick_get();set_controls(false);lv_label_set_text(lv_obj_get_child(play,0),state?"播放":"暂停");
}
void Media_Debug_List_Page(int delta) {if(viewing)return;int p=(int)list_page+delta;if(p>=0)list_page=p;draw_list();}
void Media_Debug_Files(void) {
    if(!atomic_load(&scanned)) {puts("LAB media scanning");return;}
    static const char *types[]={"JPG","PNG","BMP","AVI","MJPEG","NEEDS_CONVERT"};
    for(unsigned i=0;i<entry_count;i++)printf("LAB media file %u %s %s\n",i,types[entries[i].kind],entries[i].path);
    printf("LAB media files total=%u photos=%u videos=%u\n",entry_count,photo_count,video_count);
}
void Media_Debug_Status(void) {
    printf("LAB media ready=%d visible=%d video=%d viewing=%d scanned=%d index=%d frames=%u ms=%u paused=%d ended=%d front=%d\n",ready,visible,video_mode,viewing,atomic_load(&scanned),current,displayed_frames,displayed_ms,atomic_load(&paused),ended,front);
    printf("LAB media cache done=%u failed=%u total=%u\n",atomic_load(&cache_done),atomic_load(&cache_failed),atomic_load(&cache_total));
}

/* Bounded diagnostics upload for newly generated test/converted files only.
 * Never overwrites existing files; only this dedicated directory is writable. */
static FILE *upload;
static size_t upload_remaining;
static char upload_path[144],upload_final[128];
static size_t binary_remaining;
static uint32_t binary_crc,binary_expected;
static bool binary_failed;
static void abort_upload(void) {if(upload) {fclose(upload);upload=NULL;unlink(upload_path);}upload_remaining=0;binary_remaining=0;}
static void finish_upload(void) {
    int result=fclose(upload);upload=NULL;
    struct stat st;
    if(result || !stat(upload_final,&st) || rename(upload_path,upload_final)) {unlink(upload_path);puts("LAB upload failed");}
    else puts("LAB upload complete");
}
size_t Media_Debug_Binary(const uint8_t *data,size_t size) {
    if(!binary_remaining || !upload)return 0;
    size_t n=LV_MIN(size,binary_remaining);
    binary_crc=esp_rom_crc32_le(binary_crc,data,n);
    if(!binary_failed && fwrite(data,1,n,upload)!=n)binary_failed=true;
    binary_remaining-=n;upload_remaining-=n;
    if(!binary_remaining) {
        if(binary_failed || binary_crc!=binary_expected) {abort_upload();puts("LAB upload checksum/write failed");}
        else if(!upload_remaining)finish_upload();else puts("LAB upload data");
    }
    return n;
}
bool Media_Debug_Upload(const char *command) {
    if(!strcmp(command,"media-test-clean")) {
        static const char *names[]={"lab_landscape.jpg","lab_landscape_420.jpg","lab_portrait.png","lab_square.bmp","lab_video.avi","lab_video.mjpeg","lab_large.png","lab_broken.jpg","lab_unsupported.mp4"};
        for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {char path[128];snprintf(path,sizeof(path),"/sdcard/device_lab_media/%s",names[i]);unlink(path);}
        puts("LAB media test fixtures removed");return true;
    }
    if(!strncmp(command,"media-read-bin ",15)) {
        unsigned index,offset,bytes;
        if(!atomic_load(&scanned) || sscanf(command+15,"%u %u %u",&index,&offset,&bytes)!=3 || index>=entry_count || !bytes || bytes>262144 || offset>INT32_MAX) {puts("LAB media read invalid");return true;}
        FILE *f=fopen(entries[index].path,"rb");
        if(!f || fseek(f,offset,SEEK_SET)) {if(f)fclose(f);puts("LAB media read failed");return true;}
        uint8_t *data=heap_caps_malloc(8192,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!data) {fclose(f);puts("LAB media read failed");return true;}
        uint32_t crc=0;flockfile(stdout);
        printf("LAB MEDIA PATH %s\n",entries[index].path);
        printf("LAB MEDIA BINARY %u %u %u\n",index,offset,bytes);fflush(stdout);
        while(bytes) {
            unsigned n=LV_MIN(bytes,2048);
            if(fread(data,1,n,f)!=n)break;
            crc=esp_rom_crc32_le(crc,data,n);
            /* Bypass text VFS newline translation for binary media bytes. */
            if(usb_serial_jtag_write_bytes(data,n,pdMS_TO_TICKS(5000))!=n)break;
            bytes-=n;
        }
        printf("\nLAB MEDIA END remaining=%u crc=%08lx\n",bytes,(unsigned long)crc);fflush(stdout);funlockfile(stdout);
        free(data);fclose(f);return true;
    }
    if(!strncmp(command,"media-data-bin ",15)) {
        unsigned count,crc;
        if(!upload || sscanf(command+15,"%u %x",&count,&crc)!=2 || !count || count>3072 || count>upload_remaining) {puts("LAB upload invalid");return true;}
        binary_remaining=count;binary_crc=0;binary_expected=crc;binary_failed=false;puts("LAB upload binary ready");return true;
    }
    if(!strcmp(command,"media-stat")) {
        if(atomic_load(&scanned))for(unsigned i=0;i<entry_count;i++) {
            struct stat st;if(!stat(entries[i].path,&st))printf("LAB media stat %u %lld %s\n",i,(long long)st.st_size,entries[i].path);
        }
        puts("LAB media stat end");return true;
    }
    if(!strncmp(command,"media-read ",11)) {
        unsigned index,offset,bytes;
        if(!atomic_load(&scanned) || sscanf(command+11,"%u %u %u",&index,&offset,&bytes)!=3 || index>=entry_count || !bytes || bytes>32768 || offset>INT32_MAX) {puts("LAB media read invalid");return true;}
        FILE *f=fopen(entries[index].path,"rb");
        if(!f || fseek(f,offset,SEEK_SET)) {if(f)fclose(f);puts("LAB media read failed");return true;}
        printf("LAB MEDIA PATH %s\n",entries[index].path);
        printf("LAB MEDIA DATA %u %u %u\n",index,offset,bytes);
        while(bytes) {
            uint8_t data[256];char line[513];unsigned n=LV_MIN(bytes,sizeof(data));
            if(fread(data,1,n,f)!=n)break;
            for(unsigned i=0;i<n;i++) {static const char hex[]="0123456789abcdef";line[i*2]=hex[data[i]>>4];line[i*2+1]=hex[data[i]&15];}
            line[n*2]=0;puts(line);bytes-=n;vTaskDelay(1);
        }
        fclose(f);printf("LAB MEDIA END remaining=%u\n",bytes);return true;
    }
    if(!strncmp(command,"media-put ",10)) {
        char name[80];unsigned bytes;
        if(sscanf(command+10,"%79s %u",name,&bytes)!=2 || !bytes || bytes>134217728) {puts("LAB upload invalid");return true;}
        for(char *p=name;*p;p++)if(!isalnum((unsigned char)*p) && *p!='_' && *p!='-' && *p!='.') {puts("LAB upload bad name");return true;}
        if(strstr(name,"..")) {puts("LAB upload bad name");return true;}
        abort_upload();mkdir("/sdcard/device_lab_media",0777);snprintf(upload_final,sizeof(upload_final),"/sdcard/device_lab_media/%s",name);snprintf(upload_path,sizeof(upload_path),"%s.part",upload_final);
        struct stat st;if(!stat(upload_final,&st)) {puts("LAB upload exists");return true;}
        upload=fopen(upload_path,"wb");upload_remaining=bytes;printf("LAB upload %s\n",upload?"ready":"failed");return true;
    }
    if(!strncmp(command,"media-data ",11)) {
        const char *p=command+11;size_t len=strlen(p);uint8_t bytes[512];
        if(!upload || len%2 || len>sizeof(bytes)*2 || len/2>upload_remaining) {puts("LAB upload invalid");return true;}
        for(size_t i=0;i<len;i+=2) {
            if(!isxdigit((unsigned char)p[i]) || !isxdigit((unsigned char)p[i+1])) {abort_upload();puts("LAB upload invalid");return true;}
            unsigned value;sscanf(p+i,"%2x",&value);bytes[i/2]=value;
        }
        size_t n=len/2;if(fwrite(bytes,1,n,upload)!=n) {abort_upload();puts("LAB upload write failed");return true;}
        upload_remaining-=n;
        if(!upload_remaining)finish_upload();
        else puts("LAB upload data");
        return true;
    }
    if(!strcmp(command,"media-abort")) {abort_upload();puts("LAB upload aborted");return true;}
    return false;
}
