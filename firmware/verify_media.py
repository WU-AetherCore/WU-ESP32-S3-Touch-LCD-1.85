"""On-device media tests with actual SD reads; fixtures are created separately."""
from pathlib import Path
import re,time,serial
from device_lab_check import read_for,shot

out=Path(__file__).parent/'verification'/'media25';out.mkdir(exist_ok=True)
log=[]
s=serial.Serial();s.port='COM4';s.baudrate=115200;s.timeout=.05;s.write_timeout=5;s.dtr=False;s.rts=False;s.open()
def command(c,seconds=.25):
    s.write((c+'\n').encode());r=read_for(s,seconds);log.append(c+'\n'+r);return r
def status():
    r=command('media-status');matches=re.findall(r'LAB media ready=(.*)',r)
    assert matches,r
    return {k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)', 'ready='+matches[-1])}
def wait_ready(error=False,timeout=30):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        st=status()
        if (error and st['ended']) or (not error and st['frames']>0):return st
        time.sleep(.1)
    raise AssertionError('Media did not finish decoding: '+str(st))
def capture(name):
    for attempt in range(2):
        try:shot(s,out/(name+'.png'));return
        except RuntimeError:
            if attempt:raise
            read_for(s,1)
try:
    read_for(s,1);command('media-scan',2)
    files=command('media-files',1)
    catalogue={Path(p).name:int(i) for i,kind,p in re.findall(r'LAB media file (\d+) (\S+) (.+)',files)}
    command('page 0');assert 'missing=0' in command('audit');capture('home')
    command('page 8');assert 'missing=0' in command('audit');capture('photos')
    command('page 9');assert 'missing=0' in command('audit');capture('videos')
    for name in ['lab_landscape_420.jpg','lab_portrait.png','lab_square.bmp']:
        command(f'media-open {catalogue[name]}');wait_ready();read_for(s,3.2);capture(name.replace('.','_'))
        print('PASS photo '+name,flush=True)
    for name in ['lab_broken.jpg','lab_large.png','lab_unsupported.mp4']:
        command(f'media-open {catalogue[name]}');st=wait_ready(error=True)
        assert st['front']==-1;assert 'missing=0' in command('audit');capture(name.replace('.','_'))
        print('PASS rejected '+name,flush=True)
    command(f'media-open {catalogue["lab_video.avi"]}');wait_ready();read_for(s,1)
    a=status();assert a['frames']>=3,a
    command('media-toggle',.4);a=status();read_for(s,.7);b=status();assert a['frames']==b['frames'],(a,b)
    capture('video_paused');command('media-toggle',.5);assert status()['frames']>b['frames']
    deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        st=status()
        if st['ended']:break
    assert st['ended'] and st['frames']==72,st
    capture('video_ended');print('PASS AVI 72 frames, pause/resume/end',flush=True)
    audio=command('status');assert int(re.search(r'bytes=(\d+)',audio)[1])>=384000,audio
    command(f'media-open {catalogue["lab_video.mjpeg"]}');wait_ready()
    deadline=time.monotonic()+15
    while time.monotonic()<deadline:
        st=status()
        if st['ended']:break
    assert st['frames']==12 and st['ended'],st
    print('PASS raw MJPEG 12 frames',flush=True)
    # Cancel a large original photo repeatedly while switching into small fixtures.
    for _ in range(3):
        command('media-open 0',.08);command(f'media-open {catalogue["lab_landscape_420.jpg"]}',.08);wait_ready()
        command('media-back');command('page 0')
    command('page 8');command(f'media-open {catalogue["lab_landscape_420.jpg"]}');wait_ready()
    # A horizontal raw gesture must advance the current photo, not scroll vertically.
    for x in [270,245,215,180,145,110]:command(f'touch-raw {x} 170 1',.025)
    command('touch-raw 110 170 0');assert status()['index']!=catalogue['lab_landscape_420.jpg']
    command('page 0');command('touch-stats');command('status')
    assert not re.search(r'Guru Meditation|abort\(\)|stack overflow|Failed to allocate priv TX|spi transmit \(queue\) color failed','\n'.join(log))
    print('PASS media navigation, cancellation, horizontal gesture and no crash',flush=True)
finally:
    s.close();(out/'validation.txt').write_text('\n'.join(log),encoding='utf-8')
