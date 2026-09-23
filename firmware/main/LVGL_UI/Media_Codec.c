#include "Media_Codec.h"
#include "media_jpeg/tjpgd.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_rom_crc.h"

static uint16_t u16(const uint8_t *p) {return p[0]|((uint16_t)p[1]<<8);}
static uint32_t u32(const uint8_t *p) {return p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
typedef struct {
    FILE *file;uint32_t remaining;
    lv_color_t *pixels;unsigned crop,left,top,blocks,orientation;
    media_cancel_fn cancel;void *ctx;
} jpeg_io_t;
static size_t jpeg_read(JDEC *jd,uint8_t *buffer,size_t n) {
    jpeg_io_t *io=jd->device;
    if(io->cancel && io->cancel(io->ctx)) return 0;
    n=LV_MIN(n,io->remaining);
    size_t got=buffer?fread(buffer,1,n,io->file):(fseek(io->file,(long)n,SEEK_CUR)==0?n:0);
    io->remaining-=got;return got;
}
static int jpeg_output(JDEC *jd,void *data,JRECT *r) {
    jpeg_io_t *io=jd->device;
    if(io->cancel && io->cancel(io->ctx)) return 0;
    if(io->crop==360 && !io->left && !io->top && io->orientation==1) {
        const uint8_t *p=data;
        for(unsigned y=r->top;y<=r->bottom;y++)for(unsigned x=r->left;x<=r->right;x++,p+=3)
            io->pixels[y*360+x]=lv_color_make(p[0],p[1],p[2]);
        return 1;
    }
    int x0=LV_MAX(0,((int)r->left-(int)io->left)*360+(int)io->crop-1)/(int)io->crop;
    int y0=LV_MAX(0,((int)r->top-(int)io->top)*360+(int)io->crop-1)/(int)io->crop;
    int x1=LV_MIN(360,LV_MAX(0,((int)r->right+1-(int)io->left)*360+(int)io->crop-1)/(int)io->crop);
    int y1=LV_MIN(360,LV_MAX(0,((int)r->bottom+1-(int)io->top)*360+(int)io->crop-1)/(int)io->crop);
    unsigned stride=r->right-r->left+1;
    for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) {
        unsigned sx=io->left+x*io->crop/360,sy=io->top+y*io->crop/360;
        const uint8_t *p=(uint8_t *)data+((sy-r->top)*stride+sx-r->left)*3;
        int dx=x,dy=y;
        switch(io->orientation) {
        case 2:dx=359-x;break;case 3:dx=359-x;dy=359-y;break;case 4:dy=359-y;break;
        case 5:dx=y;dy=x;break;case 6:dx=359-y;dy=x;break;
        case 7:dx=359-y;dy=359-x;break;case 8:dx=y;dy=359-x;break;
        }
        io->pixels[dy*360+dx]=lv_color_make(p[0],p[1],p[2]);
    }
    if((++io->blocks&1023)==0) vTaskDelay(1);
    return 1;
}
bool Media_Decode_JPEG(FILE *file,uint32_t bytes,lv_color_t *pixels,unsigned *width,unsigned *height,unsigned orientation,media_cancel_fn cancel,void *ctx,char *error,size_t error_size) {
    void *pool=heap_caps_malloc(16384,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!pool) {snprintf(error,error_size,"内存不足");return false;}
    jpeg_io_t io={.file=file,.remaining=bytes,.pixels=pixels,.orientation=orientation,.cancel=cancel,.ctx=ctx};
    JDEC jd={0};JRESULT result=jd_prepare(&jd,jpeg_read,pool,16384,&io);
    if(result==JDR_OK) {
        *width=jd.width;*height=jd.height;
        if(!jd.width || !jd.height || jd.width>8192 || jd.height>8192) result=JDR_PAR;
        else {
            unsigned scale=0;while(scale<3 && (LV_MIN(jd.width,jd.height)>>(scale+1))>=360) scale++;
            unsigned w=jd.width>>scale,h=jd.height>>scale;
            io.crop=LV_MIN(w,h);io.left=(w-io.crop)/2;io.top=(h-io.crop)/2;
            result=jd_decomp(&jd,jpeg_output,scale);
        }
    }
    free(pool);
    if(result==JDR_OK && orientation>=5 && orientation<=8) {unsigned tmp=*width;*width=*height;*height=tmp;}
    if(result!=JDR_OK) snprintf(error,error_size,"JPG 解码失败：%u\n请使用普通 JPG，避免渐进式",result);
    return result==JDR_OK;
}
static uint16_t exif16(const uint8_t *p,bool le) {return le?u16(p):((uint16_t)p[0]<<8)|p[1];}
static uint32_t exif32(const uint8_t *p,bool le) {return le?u32(p):((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];}
static unsigned photo_orientation(FILE *f) {
    rewind(f);if(fgetc(f)!=0xff || fgetc(f)!=0xd8)return 1;
    for(unsigned markers=0;markers<128;markers++) {
        if(fgetc(f)!=0xff)break;
        int marker;do{marker=fgetc(f);}while(marker==0xff);
        if(marker<0 || marker==0xda || marker==0xd9)break;
        int hi=fgetc(f),lo=fgetc(f);if(hi<0 || lo<0)break;
        int n=(hi<<8|lo)-2;if(n<0)break;
        if(marker==0xe1 && n>=14) {
            uint8_t *data=heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(!data)break;
            unsigned orientation=1;
            if(fread(data,1,n,f)==n && !memcmp(data,"Exif\0\0",6)) {
                uint8_t *t=data+6;size_t size=n-6;bool le=!memcmp(t,"II",2);
                if((le || !memcmp(t,"MM",2)) && exif16(t+2,le)==42) {
                    uint32_t offset=exif32(t+4,le);
                    if(offset<=size-2) {
                        unsigned count=exif16(t+offset,le);
                        if((uint64_t)offset+2+(uint64_t)count*12<=size)for(unsigned i=0;i<count;i++) {
                            uint8_t *entry=t+offset+2+i*12;
                            if(exif16(entry,le)==0x112 && exif16(entry+2,le)==3 && exif32(entry+4,le)==1)orientation=exif16(entry+8,le);
                        }
                    }
                }
            }
            free(data);if(orientation>=2 && orientation<=8)return orientation;
        } else if(fseek(f,n,SEEK_CUR))break;
    }
    return 1;
}
bool Media_Decode_Photo(const char *path,lv_color_t *pixels,unsigned *w,unsigned *h,media_cancel_fn cancel,void *ctx,char *error,size_t error_size) {
    FILE *f=fopen(path,"rb");if(!f) {snprintf(error,error_size,"无法打开文件");return false;}
    char *file_buffer=heap_caps_malloc(16384,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(file_buffer)setvbuf(f,file_buffer,_IOFBF,16384);
    uint8_t header[54]={0};size_t count=fread(header,1,sizeof(header),f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
    bool ok=false;
    if(count>=2 && header[0]==0xff && header[1]==0xd8 && size>0) {
        unsigned orientation=photo_orientation(f);rewind(f);
        ok=Media_Decode_JPEG(f,size,pixels,w,h,orientation,cancel,ctx,error,error_size);
    }
    else if(count>=24 && !memcmp(header,"\x89PNG\r\n\x1a\n",8)) {
        if(size>0 && size<=2097152) {
            uint8_t *data=heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if(data && fread(data,1,size,f)==size) ok=Media_PNG_Decode(data,size,pixels,w,h,error,error_size);
            else snprintf(error,error_size,"读取失败或内存不足");
            free(data);
        } else snprintf(error,error_size,"PNG 文件较大，请先转换为 JPG");
    } else if(count==54 && !memcmp(header,"BM",2) && u32(header+14)>=40 && u32(header+30)==0 && (u16(header+28)==24 || u16(header+28)==32)) {
        int32_t iw=(int32_t)u32(header+18),ih=(int32_t)u32(header+22);
        if(iw>0 && iw<=8192 && ih && ih>=-8192 && ih<=8192) {
            *w=iw;*h=ih<0?-ih:ih;
            unsigned bpp=u16(header+28)/8,stride=((*w*bpp+3)/4)*4,crop=LV_MIN(*w,*h),left=(*w-crop)/2,top=(*h-crop)/2;
            uint8_t *row=heap_caps_malloc(stride,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);ok=row!=NULL;
            uint32_t offset=u32(header+10);
            if((uint64_t)offset+(uint64_t)stride**h>(uint64_t)size)ok=false;
            for(unsigned y=0;ok && y<360;y++) {
                if(cancel && cancel(ctx)) {ok=false;break;}
                unsigned sy=top+y*crop/360;
                if(ih>0)sy=*h-1-sy;
                if(fseek(f,offset+sy*stride,SEEK_SET) || fread(row,1,stride,f)!=stride) {ok=false;break;}
                for(unsigned x=0;x<360;x++) {uint8_t *p=row+(left+x*crop/360)*bpp;pixels[y*360+x]=lv_color_make(p[2],p[1],p[0]);}
            }
            free(row);
        }
        if(!ok)snprintf(error,error_size,"BMP 文件损坏或内存不足");
    } else snprintf(error,error_size,"图片格式不支持\n支持 JPG、PNG、普通 BMP");
    fclose(f);free(file_buffer);return ok;
}

typedef struct {
    uint32_t magic,version,width,height,crc,swap;
    uint64_t size,modified;
    char source[512];
} cache_header_t;
bool Media_Photo_Cached(const char *path,lv_color_t *pixels,unsigned *w,unsigned *h,media_cancel_fn cancel,void *ctx,char *error,size_t error_size,unsigned *cache_state) {
    *cache_state=0;struct stat st;
    if(stat(path,&st)) {snprintf(error,error_size,"文件已移除或无法读取");return false;}
    uint64_t hash=UINT64_C(14695981039346656037);
    for(const unsigned char *p=(const unsigned char *)path;*p;p++) {hash^=*p;hash*=UINT64_C(1099511628211);}
    char cache_path[96],part[104];snprintf(cache_path,sizeof(cache_path),"/sdcard/.lab_media_cache/%016llx.rgb",(unsigned long long)hash);snprintf(part,sizeof(part),"%s.part",cache_path);
    const size_t bytes=360*360*sizeof(lv_color_t);
    cache_header_t header;
    FILE *f=fopen(cache_path,"rb");
    if(f) {
        bool valid=fread(&header,1,sizeof(header),f)==sizeof(header) && header.magic==0x4D434831 && header.version==1 && header.swap==LV_COLOR_16_SWAP && header.size==(uint64_t)st.st_size && header.modified==(uint64_t)st.st_mtime && !strncmp(header.source,path,sizeof(header.source));
        if(valid && fread(pixels,1,bytes,f)==bytes && esp_rom_crc32_le(0,(uint8_t *)pixels,bytes)==header.crc) {
            *w=header.width;*h=header.height;*cache_state=1;fclose(f);return true;
        }
        fclose(f);
    }
    if(!Media_Decode_Photo(path,pixels,w,h,cancel,ctx,error,error_size))return false;
    if(cancel && cancel(ctx))return false;
    memset(&header,0,sizeof(header));header.magic=0x4D434831;header.version=1;header.swap=LV_COLOR_16_SWAP;header.width=*w;header.height=*h;header.size=st.st_size;header.modified=st.st_mtime;header.crc=esp_rom_crc32_le(0,(uint8_t *)pixels,bytes);snprintf(header.source,sizeof(header.source),"%s",path);
    mkdir("/sdcard/.lab_media_cache",0777);
    f=fopen(part,"wb");
    if(f) {
        bool written=fwrite(&header,1,sizeof(header),f)==sizeof(header) && fwrite(pixels,1,bytes,f)==bytes;
        if(fclose(f))written=false;
        if(written) {unlink(cache_path);if(rename(part,cache_path))unlink(part);else *cache_state=2;}else unlink(part);
    }
    return true;
}

/* RIFF sizes include payload only; each chunk has word padding. Never scan
 * arbitrary compressed data for a 'movi' string or trust unbounded chunk sizes. */
static bool avi_headers(FILE *f,uint32_t start,uint32_t end,media_avi_t *a,unsigned depth,unsigned *streams) {
    if(depth>5)return false;
    uint32_t pos=start;unsigned stream=0;bool video=false,audio=false;
    while(pos+8<=end) {
        uint8_t header[64];if(fseek(f,pos,SEEK_SET) || fread(header,1,8,f)!=8)return false;
        uint32_t n=u32(header+4),payload=pos+8;
        uint64_t next=(uint64_t)payload+n+(n&1);if(next>end)return false;
        if(!memcmp(header,"LIST",4) && n>=4) {
            if(fread(header+8,1,4,f)!=4)return false;
            if(!memcmp(header+8,"movi",4)) {a->movi_start=payload+4;a->movi_end=payload+n;}
            else if(!memcmp(header+8,"hdrl",4) || !memcmp(header+8,"strl",4)) {
                if(!avi_headers(f,payload+4,payload+n,a,depth+1,streams))return false;
            }
        } else if(!memcmp(header,"avih",4) && n>=40) {
            if(fread(header,1,40,f)!=40)return false;
            a->frame_us=u32(header);a->frames=u32(header+16);a->width=u32(header+32);a->height=u32(header+36);
        } else if(!memcmp(header,"strh",4) && n>=8) {
            if(fread(header,1,8,f)!=8)return false;
            stream=(*streams)++;video=!memcmp(header,"vids",4);audio=!memcmp(header,"auds",4);
            if(video) {
                if(memcmp(header+4,"MJPG",4) && memcmp(header+4,"mjpg",4))return false;
                a->video_stream=stream;
            }
        } else if(audio && !memcmp(header,"strf",4) && n>=16) {
            if(fread(header,1,16,f)!=16)return false;
            unsigned channels=u16(header+2),rate=u32(header+4);
            if(u16(header)==1 && u16(header+14)==16 && channels>=1 && channels<=2 && rate>=8000 && rate<=48000 && u16(header+12)==channels*2) {
                a->pcm=true;a->audio_stream=stream;a->audio_channels=channels;a->audio_rate=rate;
            }
        }
        pos=(uint32_t)next;
    }
    return true;
}
bool Media_AVI_Open(FILE *file,media_avi_t *a,char *error,size_t error_size) {
    memset(a,0,sizeof(*a));a->file=file;a->video_stream=UINT32_MAX;
    uint8_t h[12];rewind(file);if(fread(h,1,12,file)!=12 || memcmp(h,"RIFF",4) || memcmp(h+8,"AVI ",4))goto invalid;
    fseek(file,0,SEEK_END);long size=ftell(file);
    uint64_t end=(uint64_t)u32(h+4)+8;unsigned streams=0;
    if(size<12 || end>(uint64_t)size || end>INT32_MAX || !avi_headers(file,12,end,a,0,&streams) || !a->movi_start || a->video_stream==UINT32_MAX)goto invalid;
    a->next=a->movi_start;
    if(a->frame_us<16667 || a->frame_us>1000000)a->frame_us=83333;
    return true;
invalid:
    snprintf(error,error_size,"AVI 需要 MJPEG 视频编码\n请先转换为屏幕适用格式");return false;
}
int Media_AVI_Next(media_avi_t *a,uint32_t *offset,uint32_t *bytes) {
    while(a->next+8<=a->movi_end) {
        uint8_t h[12];if(fseek(a->file,a->next,SEEK_SET) || fread(h,1,8,a->file)!=8)return -1;
        uint32_t n=u32(h+4),payload=a->next+8;
        uint64_t next=(uint64_t)payload+n+(n&1);if(next>a->movi_end)return -1;
        a->next=next;
        if(!memcmp(h,"LIST",4) && n>=4) {if(fread(h+8,1,4,a->file)!=4)return -1;if(!memcmp(h+8,"rec ",4))a->next=payload+4;continue;}
        if(h[0]<'0'||h[0]>'9'||h[1]<'0'||h[1]>'9')continue;
        unsigned stream=(h[0]-'0')*10+h[1]-'0';
        *offset=payload;*bytes=n;
        if(stream==a->video_stream && h[2]=='d' && (h[3]=='c'||h[3]=='b'))return 1;
        if(a->pcm && stream==a->audio_stream && h[2]=='w' && h[3]=='b')return 2;
    }
    return 0;
}
