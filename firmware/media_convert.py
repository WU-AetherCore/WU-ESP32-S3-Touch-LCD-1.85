"""Create circular-screen media without overwriting source files.

Examples:
  python media_convert.py D:/videos/clip.mp4 --output D:/screen_media
  python media_convert.py D:/photos --output D:/screen_media --recursive
Requires FFmpeg on PATH or --ffmpeg PATH. Outputs 360x360 baseline JPG or
MJPEG AVI (12 fps, optional 16 kHz mono PCM audio), with centered cover crop.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

PHOTOS={'.jpg','.jpeg','.png','.bmp','.webp','.heic','.tif','.tiff'}
VIDEOS={'.mp4','.mov','.mkv','.avi','.webm','.3gp','.mjpg','.mjpeg'}

def find_ffmpeg(explicit=None):
    candidates=[explicit,shutil.which('ffmpeg')]
    return next((str(x) for x in candidates if x and Path(x).is_file()),None)

def convert(source,output,ffmpeg,fps=12):
    source=Path(source).resolve();output=Path(output).resolve()
    if source==output or output.exists():
        raise FileExistsError(f'Refusing to overwrite: {output}')
    output.parent.mkdir(parents=True,exist_ok=True)
    crop='scale=360:360:force_original_aspect_ratio=increase,crop=360:360,setsar=1'
    command=[ffmpeg,'-hide_banner','-nostdin','-n','-i',str(source)]
    if source.suffix.lower() in PHOTOS:
        command+=['-map','0:v:0','-vf',crop,'-frames:v','1','-c:v','mjpeg','-q:v','2','-pix_fmt','yuvj420p','-update','1']
    else:
        command+=['-map','0:v:0','-map','0:a:0?','-vf',f'fps={fps},'+crop,
            '-c:v','mjpeg','-q:v','5','-pix_fmt','yuvj420p','-c:a','pcm_s16le','-ar','16000','-ac','1']
    command += ['-threads','1',str(output)]
    try:
        subprocess.run(command,check=True)
    except BaseException:
        # Only the newly created destination is eligible for cleanup.
        if output.exists():output.unlink()
        raise
    return output

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--recursive',action='store_true');p.add_argument('--ffmpeg');p.add_argument('--fps',type=int,choices=range(5,21),default=12)
    args=p.parse_args();ffmpeg=find_ffmpeg(args.ffmpeg)
    if not ffmpeg:p.error('FFmpeg was not found; install it or pass --ffmpeg PATH.')
    root=args.source.resolve()
    if not root.exists():p.error('Source does not exist.')
    files=sorted(root.rglob('*') if args.recursive else root.glob('*')) if root.is_dir() else [root]
    count=0;failed=0
    for source in files:
        if not source.is_file() or source.suffix.lower() not in PHOTOS|VIDEOS:continue
        if args.output.resolve() in source.resolve().parents:continue
        relative=source.relative_to(root) if root.is_dir() else Path(source.name)
        # Preserve extension in stem so foo.jpg and foo.png never collide.
        destination=args.output/relative.parent/(relative.name+('.screen.jpg' if source.suffix.lower() in PHOTOS else '.screen.avi'))
        try:
            result=convert(source,destination,ffmpeg,args.fps);count+=1;print(f'Created: {result}')
        except (subprocess.CalledProcessError,FileExistsError) as exc:
            failed+=1;print(f'Skipped/failed: {source}: {exc}',file=sys.stderr)
    print(f'Converted {count}; skipped/failed {failed}. Original files unchanged.')
    return 1 if failed else 0

if __name__=='__main__':raise SystemExit(main())
