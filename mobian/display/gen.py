import re,sys
H='~/S10e/src/lineage-kernel/drivers/video/fbdev/exynos/panel/s6e3fa7/s6e3fa7_beyond0_a3_s0_panel_mdnie.h'
s=open(H).read()
def block(name):
    i=s.index('beyond0_a3_s0_'+name+'[')
    j=s.index('};',i); return s[i:j]
def bytes_of(t): return [int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})',t)]
ui=block('mdnie_ui_table')
modes={}
for m in ['DYNAMIC','STANDARD','NATURAL','AUTO']:
    i=ui.index('['+m+']'); j=ui.index('},',i); modes[m]=bytes_of(ui[i:j])
night=bytes_of(block('mdnie_night_table'))
nt=block('night_mode_table')
rows=[(bytes_of(l),re.search(r'/\* (\d+K)',l)) for l in nt.split('\n') if '0x' in l]
def emit(t,f):
    assert len(t)==196,len(t)
    f.write('s10e-dsi w f0 5a 5a\n')
    for reg,a,b in (('df',0,55),('de',55,178),('dd',178,196)):
        f.write('s10e-dsi w %s %s\n'%(reg,' '.join('%02x'%x for x in t[a:b])))
    f.write('s10e-dsi w f0 a5 a5\n')
for m,t in modes.items():
    t=list(t)
    if m!='AUTO':  # D65 modes take the coordinate-tuned D65 white (PT_5 = ff fc f6)
        t[49],t[51],t[53]=0xff,0xfc,0xf6
    emit(t,open('mode-%s.sh'%m.lower(),'w'))
import os; os.makedirs('night',exist_ok=True)
for i,(r,k) in enumerate(rows):
    t=list(night); t[31:55]=r
    emit(t,open('night/%03d.sh'%i,'w'))
print(len(rows),[k.group(1) if k else None for _,k in rows][::10])
