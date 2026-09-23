"""USB diagnostics helper: never transmits on GPIO43/44. Uses ESP-IDF Python's pyserial."""
import argparse, time, serial, struct, zlib
from pathlib import Path

def png(path,w,h,rgb):
    def chunk(tag,data):
        return struct.pack('!I',len(data))+tag+data+struct.pack('!I',zlib.crc32(tag+data)&0xffffffff)
    scan=b''.join(b'\0'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!2I5B',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(scan))+chunk(b'IEND',b''))

def read_for(s,seconds):
    end=time.monotonic()+seconds
    output=[]
    while time.monotonic()<end:
        line=s.readline()
        if line: output.append(line.decode('utf-8',errors='replace').rstrip())
    return '\n'.join(output)

def shot(s,path):
    s.write(b'shot\n')
    deadline=time.monotonic()+25
    while time.monotonic()<deadline:
        line=s.readline().decode(errors='replace').strip()
        if line.startswith('LAB SHOT '):
            _,_,w,h,swap=line.split();w,h,swap=map(int,(w,h,swap));break
    else: raise RuntimeError('Snapshot did not start')
    data=bytearray()
    while time.monotonic()<deadline:
        line=s.readline().strip()
        if line==b'LAB END SHOT':break
        if len(line)==w*4:
            try: data.extend(bytes.fromhex(line.decode()))
            except ValueError: pass
    if len(data)!=w*h*2:raise RuntimeError(f'Incomplete image: {len(data)} / {w*h*2}')
    rgb=bytearray()
    for i in range(0,len(data),2):
        v=int.from_bytes(data[i:i+2],'big' if swap else 'little')
        rgb.extend((((v>>11)&31)*255//31,((v>>5)&63)*255//63,(v&31)*255//31))
    png(path,w,h,rgb)
    print('Saved',path)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--port',default='COM4');p.add_argument('--command',action='append',default=[]);p.add_argument('--shot');p.add_argument('--seconds',type=float,default=2);p.add_argument('--log');p.add_argument('--reset',action='store_true');args=p.parse_args()
    s=serial.Serial();s.port=args.port;s.baudrate=115200;s.timeout=.2;s.write_timeout=3;s.dtr=False;s.rts=False
    s.open()
    with s:
        if args.reset:
            s.rts=True;time.sleep(.1);s.rts=False
        logs=[read_for(s,args.seconds)]
        for command in args.command:
            s.write((command+'\n').encode());logs.append(read_for(s,args.seconds))
        if args.log:Path(args.log).write_text("\n".join(logs),encoding="utf-8")
        if args.shot:shot(s,args.shot)
    text='\n'.join(logs);print(text)
    if args.log:Path(args.log).write_text(text,encoding='utf-8')
