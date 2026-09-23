"""Read MP4s from the device, convert locally, upload new AVI copies.

Default: converts the smallest source as a hardware test. Use --all to process
all supported source video extensions. For multi-GB libraries a USB card reader
and media_convert.py are much faster. Original files are never changed.
"""
import argparse
import json
from pathlib import Path
import re
import serial
import time
import zlib
from media_convert import convert,find_ffmpeg
from media_upload import upload,record_device_line

def lines_until(s,terminal,timeout=20):
    lines=[];end=time.monotonic()+timeout
    while time.monotonic()<end:
        line=s.readline().decode('utf-8',errors='replace').strip()
        record_device_line(line)
        if line:lines.append(line)
        if line==terminal:return lines
    raise TimeoutError(terminal)

def catalogue(s):
    s.write(b'media-stat\n')
    lines=lines_until(s,'LAB media stat end')
    return [{'index':int(i),'size':int(n),'path':p} for line in lines
            if (match:=re.match(r'LAB media stat (\d+) (\d+) (.+)',line))
            for i,n,p in [match.groups()]]

def download(s,item,target):
    target=Path(target);part=target.with_name(target.name+'.part')
    if target.exists():
        if target.stat().st_size!=item['size']:raise RuntimeError('Existing source copy has wrong size')
        return target
    start=part.stat().st_size if part.exists() else 0
    if start>item['size']:raise RuntimeError('Partial source copy exceeds source size')
    target.parent.mkdir(parents=True,exist_ok=True)
    with part.open('ab') as f:
        for offset in range(start,item['size'],262144):
            count=min(262144,item['size']-offset)
            s.write(f'media-read-bin {item["index"]} {offset} {count}\n'.encode())
            header=f'LAB MEDIA BINARY {item["index"]} {offset} {count}'
            lines=lines_until(s,header,15)
            paths=[x[len('LAB MEDIA PATH '):] for x in lines if x.startswith('LAB MEDIA PATH ')]
            if paths!=[item['path']]:raise RuntimeError('SD catalogue changed during transfer')
            data=bytearray();deadline=time.monotonic()+20
            while len(data)<count and time.monotonic()<deadline:
                data.extend(s.read(count-len(data)))
            if len(data)!=count:raise RuntimeError(f'Incomplete block: {len(data)}/{count}')
            trailer=''
            while time.monotonic()<deadline:
                trailer=s.readline().decode('ascii',errors='replace').strip()
                if trailer.startswith('LAB MEDIA END '):break
            expected=f'LAB MEDIA END remaining=0 crc={zlib.crc32(data):08x}'
            if trailer!=expected:raise RuntimeError('Block checksum or trailer mismatch: '+trailer)
            f.write(data);f.flush()
            if offset//262144%8==0 or offset+count==item['size']:print(f'Download {Path(item["path"]).name}: {offset+count}/{item["size"]}',flush=True)
    part.rename(target);return target

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',default='COM4');p.add_argument('--all',action='store_true');p.add_argument('--index',type=int);p.add_argument('--output',type=Path,default=Path(__file__).parent/'converted_sd_media');p.add_argument('--ffmpeg')
    p.add_argument('--keep-catalog',action='store_true',help='Use the existing scan without cancelling ongoing photo cache preparation')
    args=p.parse_args()
    ffmpeg=find_ffmpeg(args.ffmpeg)
    if not ffmpeg:p.error('FFmpeg not found')
    s=serial.Serial();s.port=args.port;s.baudrate=115200;s.timeout=.1;s.write_timeout=5;s.dtr=False;s.rts=False;s.open()
    with s:
        if not args.keep_catalog:
            s.write(b'media-scan\n');time.sleep(2);s.reset_input_buffer()
        items=[i for i in catalogue(s) if Path(i['path']).suffix.lower() in {'.mp4','.mov','.mkv','.webm','.3gp'} and '/device_lab_media/' not in i['path']]
        if args.index is not None:items=[i for i in items if i['index']==args.index]
        elif not args.all:items=sorted(items,key=lambda i:i['size'])[:1]
        else:items.sort(key=lambda i:i['size'])
        if not args.keep_catalog:
            s.write(b'page 0\n');time.sleep(.2);s.reset_input_buffer()
        results=[]
        for item in items:
            name=Path(item['path']).name
            if not re.fullmatch(r'[A-Za-z0-9_.-]+',name) or len(name+'.screen.avi')>79:
                raise ValueError('For non-ASCII or long filenames, use the card-reader converter.')
            source=download(s,item,args.output/'original_copies'/name)
            destination=args.output/(name+'.screen.avi')
            if not destination.exists():convert(source,destination,ffmpeg)
            upload(s,destination)
            results.append({**item,'copy':str(source),'converted':str(destination)})
            (args.output/'manifest.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
        s.write(b'media-scan\n')
        print(f'Completed {len(results)} video(s); source files unchanged.',flush=True)

if __name__=='__main__':main()
