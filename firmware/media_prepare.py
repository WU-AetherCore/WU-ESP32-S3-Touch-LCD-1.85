"""Precompute SD photo caches and retain on-device timing/error evidence."""
from pathlib import Path
import serial,time,re
from device_lab_check import read_for

def main():
    s=serial.Serial();s.port='COM4';s.baudrate=115200;s.timeout=.2;s.write_timeout=5;s.dtr=False;s.rts=False;s.open()
    output=Path(__file__).parent/'verification/media25/photo_cache.txt'
    with s,output.open('w',encoding='utf-8') as log:
        s.write(b'page 0\nmedia-test-clean\n');print(read_for(s,1),file=log,flush=True)
        s.write(b'media-scan\n');print(read_for(s,3),file=log,flush=True)
        s.write(b'media-prepare\n')
        deadline=time.monotonic()+7200
        while time.monotonic()<deadline:
            line=s.readline().decode('utf-8',errors='replace').strip()
            if not line:continue
            print(line,file=log,flush=True)
            if 'LAB media cache' in line:print(line,flush=True)
            match=re.search(r'LAB media cache done=(\d+) failed=(\d+) total=(\d+)',line)
            if match:
                done,failed,total=map(int,match.groups())
                if done+failed!=total:raise RuntimeError('Photo preparation cancelled')
                s.write(b'page 0\n')
                if failed:raise RuntimeError(f'{failed} photos need host conversion; see {output}')
                return
        raise TimeoutError('Photo preparation did not finish')

if __name__=='__main__':main()
