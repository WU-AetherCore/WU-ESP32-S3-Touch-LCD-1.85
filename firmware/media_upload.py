"""Upload new media via USB COM4 into /sdcard/device_lab_media.
Existing files are never overwritten; original SD card content is untouched.
"""
import argparse
import re
import serial
import time
import zlib
from pathlib import Path

def record_device_line(line):
    if line.startswith('LAB media cache '):
        target=Path(__file__).parent/'verification/media25/photo_cache.txt'
        target.parent.mkdir(parents=True,exist_ok=True)
        with target.open('a',encoding='utf-8') as f:f.write(line+'\n')

def response(s,deadline=5):
    end=time.monotonic()+deadline
    while time.monotonic()<end:
        line=s.readline().decode('utf-8',errors='replace').strip()
        record_device_line(line)
        if line.startswith('LAB upload '):return line
    raise TimeoutError('No upload response from device')

def upload(s,path,name=None):
    path=Path(path);name=name or path.name
    if not re.fullmatch(r'[A-Za-z0-9_.-]{1,79}',name) or '..' in name:
        raise ValueError('Use an ASCII filename (letters, digits, underscore, dash, dot).')
    size=path.stat().st_size
    if not 0<size<=134217728:raise ValueError('File size must be 1 byte to 128 MiB.')
    s.reset_input_buffer();s.write(f'media-put {name} {size}\n'.encode())
    answer=response(s)
    if answer=='LAB upload exists':print(f'Already exists: {name}');return False
    if answer!='LAB upload ready':raise RuntimeError(answer)
    try:
        with path.open('rb') as f:
            sent=0
            while data:=f.read(3072):
                s.write(f'media-data-bin {len(data)} {zlib.crc32(data):08x}\n'.encode()+data)
                answer=response(s)
                if answer!='LAB upload binary ready':raise RuntimeError(answer)
                answer=response(s);sent+=len(data)
                expected='LAB upload complete' if sent==size else 'LAB upload data'
                if answer!=expected:raise RuntimeError(answer)
                if sent%32768==0 or sent==size:print(f'{name}: {sent}/{size}',flush=True)
    except BaseException:
        s.write(b'media-abort\n')
        raise
    return True

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('files',nargs='+',type=Path);p.add_argument('--port',default='COM4');a=p.parse_args()
    s=serial.Serial();s.port=a.port;s.baudrate=115200;s.timeout=.1;s.write_timeout=5;s.dtr=False;s.rts=False;s.open()
    with s:
        for file in a.files:upload(s,file)
        s.write(b'media-scan\n')
