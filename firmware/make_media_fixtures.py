"""Generate deterministic decoder fixtures locally; does not access the SD card."""
from pathlib import Path
import subprocess
from media_convert import find_ffmpeg

root=Path(__file__).parent/'verification'/'media25'/'fixtures';root.mkdir(parents=True,exist_ok=True)
ffmpeg=find_ffmpeg()
def run(args):subprocess.run([ffmpeg,'-hide_banner','-loglevel','error','-y',*args],check=True)
def ppm(name,w,h):
    out=bytearray()
    for y in range(h):
        for x in range(w):
            # The central square has four quadrants; extra margins must be cropped.
            crop=min(w,h);sx=x-(w-crop)//2;sy=y-(h-crop)//2
            if sx<0 or sy<0 or sx>=crop or sy>=crop:color=(255,255,255)
            elif sx<crop//2 and sy<crop//2:color=(230,40,40)
            elif sx>=crop//2 and sy<crop//2:color=(40,210,80)
            elif sx<crop//2:color=(40,80,230)
            else:color=(240,190,30)
            out.extend(color)
    p=root/name;p.write_bytes(f'P6\n{w} {h}\n255\n'.encode()+out);return p
land=ppm('land.ppm',640,360);portrait=ppm('portrait.ppm',360,640);square=ppm('square.ppm',360,360)
for source,name in [(land,'lab_landscape.jpg'),(portrait,'lab_portrait.png'),(square,'lab_square.bmp')]:
    run(['-i',str(source),'-frames:v','1','-threads','1',str(root/name)])
run(['-f','lavfi','-i','testsrc2=size=360x360:rate=12','-f','lavfi','-i','sine=frequency=600:sample_rate=16000','-t','6','-c:v','mjpeg','-q:v','8','-pix_fmt','yuvj420p','-c:a','pcm_s16le','-ar','16000','-ac','1','-threads','1',str(root/'lab_video.avi')])
run(['-i',str(root/'lab_video.avi'),'-t','1','-an','-c:v','copy','-f','mjpeg',str(root/'lab_video.mjpeg')])
run(['-f','lavfi','-i','color=c=red:size=1024x1024','-frames:v','1','-threads','1',str(root/'lab_large.png')])
run(['-i',str(root/'lab_video.avi'),'-t','1','-an','-c:v','mpeg4','-threads','1',str(root/'lab_unsupported.mp4')])
(root/'lab_broken.jpg').write_bytes((root/'lab_landscape.jpg').read_bytes()[:80])
print('Fixtures:',root)
