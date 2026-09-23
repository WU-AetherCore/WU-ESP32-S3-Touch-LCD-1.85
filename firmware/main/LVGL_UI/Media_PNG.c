/* Use the bundled LodePNG decoder with PSRAM allocation, independently of the
 * LVGL image cache and its small GUI heap. Original license remains in lodepng.c. */
#define LV_USE_PNG 1
#define LODEPNG_NO_COMPILE_ALLOCATORS
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#include "esp_heap_caps.h"
#include <stdlib.h>
void *lodepng_malloc(size_t n) {return n<=2097152?heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;}
void *lodepng_realloc(void *p,size_t n) {return n<=2097152?heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;}
void lodepng_free(void *p) {free(p);}
#include "src/extra/libs/png/lodepng.c"
#include "Media_Codec.h"
bool Media_PNG_Decode(const uint8_t *data,size_t size,lv_color_t *pixels,unsigned *w,unsigned *h,char *error,size_t error_size) {
    LodePNGState state;lodepng_state_init(&state);
    unsigned err=lodepng_inspect(w,h,&state,data,size);
    if(!err && (!*w || !*h || (uint64_t)*w**h>262144)) {
        snprintf(error,error_size,"PNG 图片较大，请先转换为 JPG");lodepng_state_cleanup(&state);return false;
    }
    state.decoder.zlibsettings.max_output_size=2097152;
    unsigned char *rgba=NULL;
    if(!err) err=lodepng_decode(&rgba,w,h,&state,data,size);
    if(!err) {
        unsigned crop=LV_MIN(*w,*h),left=(*w-crop)/2,top=(*h-crop)/2;
        for(unsigned y=0;y<360;y++) for(unsigned x=0;x<360;x++) {
            unsigned sx=left+x*crop/360,sy=top+y*crop/360;
            const uint8_t *p=rgba+((size_t)sy**w+sx)*4;
            pixels[y*360+x]=lv_color_make(p[0]*p[3]/255,p[1]*p[3]/255,p[2]*p[3]/255);
        }
    } else snprintf(error,error_size,"PNG 解码失败：%u",err);
    free(rgba);lodepng_state_cleanup(&state);return err==0;
}
