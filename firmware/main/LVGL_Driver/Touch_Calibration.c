#include "Touch_Calibration.h"
#include "nvs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Calibration measures the installed panel, including its physical orientation.
 * Three points fit an affine transform; two independent points must validate it. */
typedef struct { uint32_t version; float x[3], y[3]; } mapping_t;
static mapping_t mapping, candidate;
static const lv_point_t targets[]={{80,115},{280,115},{180,275},{110,220},{250,220}};
static float samples[3][2];
static lv_obj_t *screen, *previous, *dot, *caption;
static lv_timer_t *timeout;
static unsigned step, count;
static int sum_x,sum_y,first_x,first_y;
static uint32_t down_at;
static bool was_down, unstable, finishing;
extern const lv_font_t font_music_cjk;

void Touch_Calibration_Init(void) {
    nvs_handle_t handle;
    if(nvs_open("touch",NVS_READONLY,&handle)==ESP_OK) {
        size_t size=sizeof(mapping);
        if(nvs_get_blob(handle,"map_v1",&mapping,&size)!=ESP_OK || size!=sizeof(mapping) || mapping.version!=1)
            memset(&mapping,0,sizeof(mapping));
        nvs_close(handle);
    }
    printf("LAB touch calibration loaded=%d\n",mapping.version==1);
}
static void apply(const mapping_t *m,int x,int y,lv_point_t *p) {
    p->x=(lv_coord_t)lroundf(m->x[0]*x+m->x[1]*y+m->x[2]);
    p->y=(lv_coord_t)lroundf(m->y[0]*x+m->y[1]*y+m->y[2]);
}
void Touch_Map(int x,int y,lv_point_t *p) {
    if(mapping.version==1) apply(&mapping,x,y,p);
    else {p->x=x;p->y=y;}
    p->x=LV_CLAMP(0,p->x,359);p->y=LV_CLAMP(0,p->y,359);
}
static bool fit(void) {
    float ux=samples[1][0]-samples[0][0],uy=samples[1][1]-samples[0][1];
    float vx=samples[2][0]-samples[0][0],vy=samples[2][1]-samples[0][1];
    float det=ux*vy-uy*vx;
    if(fabsf(det)<1000) return false;
    for(int axis=0;axis<2;axis++) {
        float *c=axis?candidate.y:candidate.x;
        float a=axis?targets[1].y-targets[0].y:targets[1].x-targets[0].x;
        float b=axis?targets[2].y-targets[0].y:targets[2].x-targets[0].x;
        c[0]=(a*vy-b*uy)/det;c[1]=(ux*b-vx*a)/det;
        c[2]=(axis?targets[0].y:targets[0].x)-c[0]*samples[0][0]-c[1]*samples[0][1];
        float norm=hypotf(c[0],c[1]);
        if(!isfinite(norm) || norm<0.5f || norm>1.5f) return false;
    }
    candidate.version=1;return true;
}
static void update_target(const char *message) {
    lv_obj_set_pos(dot,targets[step].x-15,targets[step].y-15);
    lv_obj_set_style_bg_color(dot,lv_color_hex(0x47DECB),0);
    char text[160];snprintf(text,sizeof(text),"%s\n%u / 5",message,step+1);
    lv_label_set_text(caption,text);
}
static void finish(void *unused) {
    if(!screen) return;
    lv_scr_load(previous);lv_obj_del(screen);screen=NULL;
    if(timeout) {lv_timer_del(timeout);timeout=NULL;}
    finishing=false;was_down=false;
}
void Touch_Calibration_Cancel(void) {
    if(screen && !finishing) {finishing=true;lv_async_call(finish,NULL);puts("LAB touch calibration cancelled");}
}
static void expire(lv_timer_t *timer) {Touch_Calibration_Cancel();}
void Touch_Calibration_Start(void) {
    if(screen) return;
    previous=lv_scr_act();step=count=0;was_down=finishing=false;
    screen=lv_obj_create(NULL);lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,lv_color_hex(0x0B101A),0);
    lv_obj_set_style_text_font(screen,&font_music_cjk,0);
    lv_obj_set_style_text_color(screen,lv_color_white(),0);
    lv_obj_t *title=lv_label_create(screen);lv_label_set_text(title,"触摸校准");lv_obj_align(title,LV_ALIGN_TOP_MID,0,45);
    caption=lv_label_create(screen);lv_obj_set_width(caption,240);lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);lv_obj_align(caption,LV_ALIGN_CENTER,0,-5);
    dot=lv_obj_create(screen);lv_obj_remove_style_all(dot);lv_obj_set_size(dot,30,30);lv_obj_set_style_radius(dot,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(dot,LV_OPA_COVER,0);
    lv_obj_t *center=lv_obj_create(dot);lv_obj_remove_style_all(center);lv_obj_set_size(center,6,6);lv_obj_set_style_bg_color(center,lv_color_white(),0);lv_obj_set_style_bg_opa(center,LV_OPA_COVER,0);lv_obj_set_style_radius(center,LV_RADIUS_CIRCLE,0);lv_obj_center(center);
    update_target("按住圆点中心后松开");lv_scr_load(screen);
    timeout=lv_timer_create(expire,180000,NULL);
    puts("LAB touch calibration started: hold and release each of 5 targets");
}
bool Touch_Calibration_Sample(bool pressed,int x,int y) {
    if(!screen) return false;
    if(finishing) return true;
    if(pressed) {
        if(!was_down) {count=0;sum_x=sum_y=0;first_x=x;first_y=y;down_at=lv_tick_get();unstable=false;}
        if(abs(x-first_x)>15 || abs(y-first_y)>15) unstable=true;
        if(count<1000) {sum_x+=x;sum_y+=y;count++;}
        was_down=true;
        lv_obj_set_style_bg_color(dot,lv_color_hex(0xFFB84D),0);
    } else if(was_down) {
        was_down=false;
        if(count<3 || lv_tick_elaps(down_at)<50 || unstable) {update_target("请按住圆点，保持手指稳定");return true;}
        x=sum_x/(int)count;y=sum_y/(int)count;
        printf("LAB touch calibration point=%u raw=%d,%d target=%d,%d samples=%u\n",step+1,x,y,targets[step].x,targets[step].y,count);
        if(step<3) {samples[step][0]=x;samples[step][1]=y;}
        if(step==2 && !fit()) {step=0;update_target("位置异常，请重新点击圆点");return true;}
        if(step>=3) {
            lv_point_t p;apply(&candidate,x,y,&p);
            int dx=p.x-targets[step].x,dy=p.y-targets[step].y;
            printf("LAB touch validation error=%d,%d\n",dx,dy);
            if(dx*dx+dy*dy>18*18) {step=0;update_target("校准偏差较大，请重新点击");return true;}
        }
        if(++step==5) {
            nvs_handle_t handle;esp_err_t err=nvs_open("touch",NVS_READWRITE,&handle);
            if(err==ESP_OK) {err=nvs_set_blob(handle,"map_v1",&candidate,sizeof(candidate));if(err==ESP_OK) err=nvs_commit(handle);nvs_close(handle);}
            if(err!=ESP_OK) {step=0;update_target("保存失败，请重新校准");return true;}
            mapping=candidate;
            printf("LAB touch calibration saved x=%.5f,%.5f,%.3f y=%.5f,%.5f,%.3f\n",mapping.x[0],mapping.x[1],mapping.x[2],mapping.y[0],mapping.y[1],mapping.y[2]);
            finishing=true;lv_async_call(finish,NULL);
        } else update_target(step>=3?"再次点击圆点，验证位置":"按住圆点中心后松开");
    }
    return true;
}
