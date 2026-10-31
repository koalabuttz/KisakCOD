"""Optional real-asset X11 menu interaction test; requires Pillow and Xvfb/xdotool.
Runs actual engine input/menu logic. Assets and localization remain caller inputs.
SDL dummy audio explicitly tests lifecycle, not physical audibility.
"""
import argparse,os,re,sys,json,select,subprocess,signal
from pathlib import Path
from test_linux_window_x11 import wait_for,request_window_close
from x11_pixel_checks import read_pixels
from linux_menu_pixel_reference import reference
parser=argparse.ArgumentParser(description=__doc__)
for option in ('binary','bundle','assets','menu-inventory','output'):
 parser.add_argument('--'+option,required=True,type=Path)
parser.add_argument('--emulator',help='Optional executable wrapper, such as qemu-i386')
parser.add_argument('--xvfb',default='Xvfb')
parser.add_argument('--xdotool',default='xdotool')
args=parser.parse_args()
binary=args.binary.resolve();bundle=args.bundle.resolve();assets=args.assets.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
mode='test'
inventory=json.loads(args.menu_inventory.read_text());menu=next(m for m in inventory['menus'] if m['window']['name']=='options_look')
atlas=(assets/'gamefonts_pc.iwi').read_bytes()
rfd,wfd=os.pipe();processes=[];evidence=[]
try:
 with (out/'xvfb.log').open('w') as log:
  xvfb=subprocess.Popen([args.xvfb,'-displayfd',str(wfd),'-screen','0','1200x900x24','-nolisten','tcp'],pass_fds=(wfd,),stdout=log,stderr=subprocess.STDOUT);processes.append(xvfb)
 os.close(wfd);wfd=None;assert select.select([rfd],[],[],10)[0];display=':'+os.read(rfd,64).decode().strip()
 env=dict(os.environ,DISPLAY=display,SDL_VIDEODRIVER='x11',SDL_AUDIODRIVER='dummy')
 tenv=dict(env)
 def xdo(*values):return subprocess.check_output([args.xdotool,*map(str,values)],env=tenv,text=True,timeout=10).strip()
 def launch(name,controls=True):
  title='KisakControls-'+mode+'-'+name;trace=out/(name+'.log')
  command=([args.emulator] if args.emulator else [])+[str(binary),'--title',title,'--trace','--run-ms','60000']
  if controls:command+=['--menu-controls',str(bundle),'--ui-asset-dir',str(assets)]
  with trace.open('w') as log:p=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT)
  processes.append(p);wait_for(lambda:'window=ready' in trace.read_text(),20,name+' ready',p)
  wid=int(xdo('search','--onlyvisible','--name','^'+title+'$'));return p,wid,trace
 def state(trace):
  lines=[x for x in trace.read_text().splitlines() if x.startswith('menu_state ') and 'phase=present' in x]
  return dict(re.findall(r'(\w+)=([^ ]+)',lines[-1])) if lines else {}
 def expect(trace,p,**values):
  want={k:str(v) for k,v in values.items()};wait_for(lambda:all(state(trace).get(k)==v for k,v in want.items()),20,'menu state '+str(want),p)
  got=state(trace);evidence.append(got);return got
 p,wid,trace=launch('main');xdo('windowmove',wid,10,10);xdo('windowraise',wid);xdo('windowfocus','--sync',wid)
 expect(trace,p,open=1,width=640,height=480)
 for width,height in [(640,480),(900,675),(720,540),(960,540),(320,240)]:
  xdo('windowsize',wid,width,height);expect(trace,p,open=1,width=width,height=height)
  want,positions,layout=reference(bundle,menu,atlas,width,height);got=read_pixels(display,wid,positions)
  for sample in got['samples'].values():
   sample['expected_rgb']=want[sample['x'],sample['y']];assert max(abs(a-b) for a,b in zip(sample['rgb'],sample['expected_rgb']))<=3,(sample,layout)
  got['reference_layout']=layout;evidence.append({'pixels':got})
 xdo('windowsize',wid,640,480);expect(trace,p,width=640,height=480)
 xdo('mousemove','--window',wid,20,20);xdo('key','Tab')
 wait_for(lambda:state(trace).get('focus') in ('106','112'),20,'keyboard selects an eligible control',p)
 if state(trace)['focus']=='112':xdo('key','Up')
 s=expect(trace,p,focus=106);before=int(s['cl_freelook'])
 xdo('key','Return');expect(trace,p,cl_freelook=1-before)
 xdo('key','Down');s=expect(trace,p,focus=112);before=int(s['m_filter']);xdo('key','Return');expect(trace,p,m_filter=1-before)
 xdo('mousemove','--window',wid,450,260);s=expect(trace,p,focus=106);before=int(s['cl_freelook']);xdo('click',1);expect(trace,p,cl_freelook=1-before)
 xdo('key','Escape');expect(trace,p,open=0);assert p.poll() is None
 xdo('type','--clearmodifiers','--delay','15','menu_open');xdo('key','Return');expect(trace,p,open=1)
 peer,pwid,ptrace=launch('peer',False);xdo('windowmove',pwid,800,600);xdo('windowfocus','--sync',wid);xdo('keydown','w')
 offset=len(trace.read_text());xdo('windowfocus','--sync',pwid)
 wait_for(lambda:'focus lost' in trace.read_text()[offset:] and 'input=reset held=0' in trace.read_text()[offset:],20,'held key reset on focusloss',p)
 xdo('keyup','w');xdo('windowfocus','--sync',wid);xdo('key','Tab');expect(trace,p,focus=112)
 xdo('keydown','w');xdo('windowsize',wid,720,540);expect(trace,p,width=720,height=540);xdo('keyup','w')
 request_window_close(display,wid);assert p.wait(timeout=20)==0;assert re.search(r'window=closed .*held=0 .*game=false',trace.read_text())
 request_window_close(display,pwid);assert peer.wait(timeout=20)==0
 for n in range(2):
  p,wid,trace=launch('recreate'+str(n));expect(trace,p,open=1);xdo('windowfocus','--sync',wid);xdo('keydown','w')
  if n:p.send_signal(signal.SIGTERM)
  else:request_window_close(display,wid)
  assert p.wait(timeout=20)==0;xdo('keyup','w');assert re.search(r'window=closed .*held=0 .*game=false',trace.read_text())
 (out/'evidence.json').write_text(json.dumps({'audio':'explicit SDL dummy driver; no physical audibility claim','checks':evidence},indent=2));print(mode,'genuine menu interaction/lifecycle and independent labels PASS')
finally:
 os.close(rfd)
 if wfd is not None:os.close(wfd)
 for p in reversed(processes):
  if p.poll() is None:
   p.terminate()
   try:p.wait(timeout=10)
   except subprocess.TimeoutExpired:p.kill();p.wait()
