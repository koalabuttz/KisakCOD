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
parser.add_argument('--composition',type=Path,help='Exported genuine window/cursor metadata for composition checks')
parser.add_argument('--emulator',help='Optional executable wrapper, such as qemu-i386')
parser.add_argument('--xvfb',default='Xvfb')
parser.add_argument('--xdotool',default='xdotool')
args=parser.parse_args()
binary=args.binary.resolve();bundle=args.bundle.resolve();assets=args.assets.resolve();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
mode='test'
inventory=json.loads(args.menu_inventory.read_text());menu=next(m for m in inventory['menus'] if m['window']['name']=='options_look')
atlas=(assets/'gamefonts_pc.iwi').read_bytes()
composition=json.loads(args.composition.read_text()) if args.composition else None
if composition:
 from linux_menu_composition_reference import CompositionReference
rfd,wfd=os.pipe();processes=[];evidence=[];client_directories=[]
try:
 with (out/'xvfb.log').open('w') as log:
  xvfb=subprocess.Popen([args.xvfb,'-displayfd',str(wfd),'-screen','0','1200x900x24','-nolisten','tcp'],pass_fds=(wfd,),stdout=log,stderr=subprocess.STDOUT);processes.append(xvfb)
 os.close(wfd);wfd=None;assert select.select([rfd],[],[],10)[0];display=':'+os.read(rfd,64).decode().strip()
 env=dict(os.environ,DISPLAY=display,SDL_VIDEODRIVER='x11',SDL_AUDIODRIVER='dummy')
 tenv=dict(env)
 def xdo(*values):return subprocess.check_output([args.xdotool,*map(str,values)],env=tenv,text=True,timeout=10).strip()
 startup={'display':display,'xvfb_pid':xvfb.pid,'geometry_probes':[]}
 def display_ready():
  probe=subprocess.run([args.xdotool,'getdisplaygeometry'],env=tenv,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=5)
  startup['geometry_probes'].append({'exit':probe.returncode,'output':probe.stdout})
  (out/'x11-startup.json').write_text(json.dumps(startup,indent=2))
  return probe.returncode==0
 # Check the test display independently. Do not retry a failed application.
 wait_for(display_ready,10,'private X11 display accepts connections',xvfb)
 def launch(name,controls=True):
  title='KisakControls-'+mode+'-'+name;trace=out/(name+'.log')
  isolated=out/(name+'-cwd');isolated.mkdir(exist_ok=True);assert not list(isolated.iterdir()),'client test directory must be empty'
  client_directories.append(isolated)
  command=([args.emulator] if args.emulator else [])+[str(binary),'--title',title,'--trace']
  # Full-frame software composition under Debug/QEMU can exceed one minute.
  # Each interaction remains bounded; the harness always closes child processes.
  if not composition:command+=['--run-ms','60000']
  if controls:command+=['--menu-controls',str(bundle),'--ui-asset-dir',str(assets)]
  with trace.open('w') as log:p=subprocess.Popen(command,cwd=isolated,env=env,stdout=log,stderr=subprocess.STDOUT)
  processes.append(p);wait_for(lambda:'window=ready' in trace.read_text(),20,name+' ready',p)
  if composition and controls:
   assert 'menu_traversal painted=1 item_count=12 blur_requested=7 blur_rendered=0 compositor=partial' in trace.read_text(), 'actual traversal/compositor boundary missing'
  wid=int(xdo('search','--onlyvisible','--name','^'+title+'$'));return p,wid,trace
 def state(trace):
  lines=[x for x in trace.read_text().splitlines() if x.startswith('menu_state ') and 'phase=present' in x]
  return dict(re.findall(r'(\w+)=([^ ]+)',lines[-1])) if lines else {}
 def expect(trace,p,**values):
  want={k:str(v) for k,v in values.items()};wait_for(lambda:all(state(trace).get(k)==v for k,v in want.items()),20,'menu state '+str(want),p)
  got=state(trace);evidence.append(got);return got
 cursor_references={}
 def cursor_check(p,wid,trace,width,height,mx,my,minimum=8,move=True,order=False):
  if move:xdo('mousemove','--window',wid,mx,my)
  expect(trace,p,cursor_visible=1,pointer_inside=1,window_focused=1,pointer_x=mx,pointer_y=my)
  if (width,height) not in cursor_references:
   oracle=CompositionReference(composition,assets,width,height)
   labels,_,_=reference(bundle,menu,atlas,width,height,oracle.background)
   cursor_references[width,height]=oracle,labels
  oracle,labels=cursor_references[width,height]
  sx,sy=oracle.sx,oracle.sy
  points=[(x,y) for y in range(max(0,int(my-16*sy)-2),min(height,int(my+16*sy)+3))
                for x in range(max(0,int(mx-16*sx)-2),min(width,int(mx+16*sx)+3))]
  changed=[point for point in points if max(abs(a-b) for a,b in zip(oracle.cursor(*point,mx,my,labels.get(point,oracle.background(*point))),labels.get(point,oracle.background(*point))))>8]
  assert len(changed)>=minimum,('cursor oracle lacks visible samples',width,height,mx,my,len(changed))
  selected=changed[::max(1,len(changed)//100)]+points[::max(1,len(points)//30)]
  under={}
  if order:
   under,_,_=reference(bundle,menu,atlas,width,height,lambda x,y:oracle.cursor(x,y,mx,my))
   distinguishing=[point for point in points if max(abs(a-b) for a,b in zip(oracle.cursor(*point,mx,my,labels.get(point,oracle.background(*point))),under.get(point,oracle.cursor(*point,mx,my))))>6]
   assert distinguishing,('cursor/text draw-order oracle has no distinguishing overlap',mx,my)
   selected+=distinguishing[:20]
  got=read_pixels(display,wid,[(f'cursor{i}',*point) for i,point in enumerate(selected)])
  negatives=0
  for value in got['samples'].values():
   point=value['x'],value['y'];old=labels.get(point,oracle.background(*point));wanted=oracle.cursor(*point,mx,my,old)
   value['expected_rgb']=wanted
   assert max(abs(a-b) for a,b in zip(value['rgb'],wanted))<=3,('cursor composition',value,(width,height,mx,my))
   negatives+=max(abs(a-b) for a,b in zip(value['rgb'],old))>5
  assert negatives>=minimum,('missing cursor negative control',negatives)
  wrong_order=0
  if order:
   wrong_order=sum(max(abs(a-b) for a,b in zip(value['rgb'],under.get((value['x'],value['y']),oracle.cursor(value['x'],value['y'],mx,my))))>3 for value in got['samples'].values())
   assert wrong_order>0,'readback must reject cursor-under-text ordering'
  evidence.append({'cursor_pixels':got,'no_cursor_negative_mismatches':negatives,'cursor_under_text_negative_mismatches':wrong_order,'pointer':[mx,my]})
 p,wid,trace=launch('main');xdo('windowmove',wid,10,10);xdo('windowraise',wid);xdo('windowfocus','--sync',wid)
 expect(trace,p,open=1,width=640,height=480)
 sizes=[(640,480),(900,675),(720,540),(960,540),(320,240)]
 if composition:sizes[1:1]=[(660,500),(480,640)]
 for width,height in sizes:
  xdo('windowsize',wid,width,height);expect(trace,p,open=1,width=width,height=height)
  oracle=CompositionReference(composition,assets,width,height) if composition else None
  if composition:
   xdo('mousemove',1190,890);expect(trace,p,cursor_visible=0,pointer_inside=0)
  want,positions,layout=reference(bundle,menu,atlas,width,height,oracle.background if oracle else None);got=read_pixels(display,wid,positions)
  for sample in got['samples'].values():
   sample['expected_rgb']=want[sample['x'],sample['y']];assert max(abs(a-b) for a,b in zip(sample['rgb'],sample['expected_rgb']))<=3,(sample,layout)
  got['reference_layout']=layout;evidence.append({'pixels':got})
  if composition:
   # Genuine background layers include negative-height/width mirrored caps.
   positions=[(f'background{i}',x,y) for i,(x,y) in enumerate([(5,5),(width//2,5),(width-5,5),(5,height-5),(width//2,height-5),(width-5,height-5),(width//2,height//2)])]
   picture=read_pixels(display,wid,positions)
   for value in picture['samples'].values():
    wanted=oracle.background(value['x'],value['y']);value['expected_rgb']=wanted
    assert max(abs(a-b) for a,b in zip(value['rgb'],wanted))<=3,('background composition',value,width,height)
   evidence.append({'background_pixels':picture})
   cursor_check(p,wid,trace,width,height,width//3,height//3)
   cursor_check(p,wid,trace,width,height,1,1)
   cursor_check(p,wid,trace,width,height,width-8,height-8,minimum=1)
   # Cursor overlaps real label glyphs, proving it is composed after text.
   labelpoint=next((sample['x'],sample['y']) for sample in got['samples'].values() if sample['x']>width//8 and sample['y']>height//3)
   cursor_check(p,wid,trace,width,height,*labelpoint,order=True)
   # Keep pointer stationary while changing dimensions; it must not drift.
   xdo('mousemove','--window',wid,20,20);expect(trace,p,pointer_x=20,pointer_y=20,cursor_visible=1)
   xdo('windowsize',wid,width+20,height);expect(trace,p,width=width+20,height=height)
   cursor_check(p,wid,trace,width+20,height,20,20,move=False)
   xdo('windowsize',wid,width,height);expect(trace,p,width=width,height=height)
 xdo('windowsize',wid,640,480);expect(trace,p,width=640,height=480)
 xdo('mousemove','--window',wid,20,20);xdo('key','Tab')
 wait_for(lambda:state(trace).get('focus') in ('106','112'),20,'keyboard selects an eligible control',p)
 if state(trace)['focus']=='112':xdo('key','Up')
 s=expect(trace,p,focus=106);before=int(s['cl_freelook'])
 xdo('key','Return');expect(trace,p,cl_freelook=1-before)
 xdo('key','Down');s=expect(trace,p,focus=112);before=int(s['m_filter']);xdo('key','Return');expect(trace,p,m_filter=1-before)
 xdo('mousemove','--window',wid,450,260);s=expect(trace,p,focus=106);before=int(s['cl_freelook']);xdo('click',1);expect(trace,p,cl_freelook=1-before)
 preserved={key:state(trace)[key] for key in ('cl_freelook','m_filter')}
 for cycle in range(5):
  xdo('key','Escape');expect(trace,p,open=0,**preserved,**({'cursor_visible':0} if composition else {}));assert p.poll() is None
  xdo('type','--clearmodifiers','--delay','15','menu_open');xdo('key','Return');expect(trace,p,open=1,**preserved)
 # Hold a real X key until autorepeat reaches the production menu consumer.
 xdo('mousemove','--window',wid,450,260);s=expect(trace,p,focus=106);before=int(s['cl_freelook']);offset=len(trace.read_text())
 xdo('keydown','Return')
 wait_for(lambda:trace.read_text()[offset:].count('key=13 down=1\n')>=4,5,'real keyboard autorepeat',p)
 xdo('keyup','Return');wait_for(lambda:'key=13 down=0\n' in trace.read_text()[offset:],5,'autorepeat release',p)
 repeat_log=trace.read_text()[offset:];repeats=repeat_log.count('key=13 down=1\n')
 # CL_KeyEvent tracks repeat events but admits only the first UI activation.
 assert repeat_log.count('menu_sound alias=mouse_click ')==1
 expect(trace,p,cl_freelook=1-before,held=0)
 xdo('key','Return');expect(trace,p,cl_freelook=before,held=0)
 evidence.append({'actual_key_repeat_presses':repeats,'same_process_reopen_cycles':5})
 if composition:cursor_check(p,wid,trace,640,480,20,20)
 peer,pwid,ptrace=launch('peer',False);xdo('windowmove',pwid,800,600);xdo('windowfocus','--sync',wid);xdo('keydown','w')
 offset=len(trace.read_text());xdo('windowfocus','--sync',pwid)
 wait_for(lambda:'focus lost' in trace.read_text()[offset:] and 'input=reset held=0' in trace.read_text()[offset:],20,'held key reset on focusloss',p)
 if composition:
  expect(trace,p,window_focused=0,cursor_visible=0,held=0)
  oracle=CompositionReference(composition,assets,640,480)
  points=[(x,y) for x in range(20,36) for y in range(20,36) if max(abs(a-b) for a,b in zip(oracle.cursor(x,y,20,20),oracle.background(x,y)))>8]
  assert len(points)>=8
  got=read_pixels(display,wid,[(f'focus_hidden{i}',*point) for i,point in enumerate(points)])
  for value in got['samples'].values():
   wanted=oracle.background(value['x'],value['y']);value['expected_rgb']=wanted
   assert max(abs(a-b) for a,b in zip(value['rgb'],wanted))<=3,('focus loss must remove drawn cursor',value)
  evidence.append({'focus_loss_pixels':got})
 xdo('keyup','w');xdo('windowfocus','--sync',wid)
 if composition:cursor_check(p,wid,trace,640,480,20,20,move=False)
 xdo('key','Tab');expect(trace,p,focus=112)
 xdo('keydown','w');xdo('windowsize',wid,720,540);expect(trace,p,width=720,height=540);xdo('keyup','w')
 request_window_close(display,wid);assert p.wait(timeout=20)==0;assert re.search(r'window=closed .*held=0 .*game=false',trace.read_text())
 request_window_close(display,pwid);assert peer.wait(timeout=20)==0
 for n in range(2):
  p,wid,trace=launch('recreate'+str(n));expect(trace,p,open=1,cl_freelook=1,m_filter=0);xdo('windowfocus','--sync',wid)
  xdo('mousemove','--window',wid,450,260);expect(trace,p,focus=106);xdo('click',1);expect(trace,p,cl_freelook=0)
  xdo('keydown','w')
  if n:p.send_signal(signal.SIGTERM)
  else:request_window_close(display,wid)
  assert p.wait(timeout=20)==0;xdo('keyup','w');assert re.search(r'window=closed .*held=0 .*game=false',trace.read_text())
 for directory in client_directories:
  assert not list(directory.rglob('*')), ('unexpected client filesystem output',directory)
 evidence.append({'isolated_client_files':0,'processes':len(client_directories)})
 (out/'evidence.json').write_text(json.dumps({'audio':'explicit SDL dummy driver; no physical audibility claim','checks':evidence},indent=2));print(mode,'genuine menu interaction/lifecycle and independent labels PASS')
finally:
 os.close(rfd)
 if wfd is not None:os.close(wfd)
 for p in reversed(processes):
  if p.poll() is None:
   p.terminate()
   try:p.wait(timeout=10)
   except subprocess.TimeoutExpired:p.kill();p.wait()
