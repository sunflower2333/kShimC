#!/usr/bin/env python3
"""Generate selected-only design descriptors; --check verifies the committed header."""
from pathlib import Path
import argparse
import json
import re

ROOT = Path(__file__).resolve().parents[1]
KINDS = ['CLASSIC','CARDS','TERMINAL','MINIMAL','HIGH_CONTRAST','FLUENT','FLUENT2','MATERIAL2','MATERIAL3','SURFACE','CUPERTINO','IOS_HIG','HARMONYOS','CLOVER','GLASS','AURORA','SOFT_UI','BENTO','NEON','NORD']
LAYOUTS = ['LIST','GRID','BENTO','SPLIT','RIBBON','INSET']
MARKS = ['CURSOR','DOT','CHECK','RADIO','BAR','PILL','CORNERS']
BASE = dict(enabled=1,kind='MINIMAL',layout='LIST',mark='CHECK',max_width=760,padding=24,
            title_size=28,row_size=18,status_size=14,row_height=56,tile_height=144,tile_width=180,
            gap=8,panel_radius=0,row_radius=8,boot_radius=8,boot_width=160,boot_height=44,
            boot_align=2,icon_size=0,header=0,panel_shadow=0,row_shadow=0,focus_outline=0,
            focus_glow=0,press_scale=254,grid_two=560,grid_three=900,background=0)

def preset(name, **values):
    item = BASE.copy()
    item.update(kind=name, title='Choose a system', action='Boot', **values)
    return item

DATA = {
'CLASSIC':preset('CLASSIC',mark='CURSOR',max_width=720,padding=20,title_size=18,row_size=17,status_size=13,row_height=38,gap=0,row_radius=0,boot_radius=0,boot_width=136,boot_height=38,header=1,press_scale=256),
'CARDS':preset('CARDS',layout='GRID',max_width=1000,padding=28,title_size=28,row_height=56,tile_height=136,gap=16,row_radius=12,boot_width=180,icon_size=32,row_shadow=6,focus_outline=2,grid_two=520,grid_three=840,status_size=13),
'TERMINAL':preset('TERMINAL',mark='CURSOR',max_width=880,padding=20,title_size=20,row_size=16,row_height=36,gap=0,row_radius=0,boot_radius=0,boot_width=176,boot_height=36,boot_align=0,header=3,press_scale=256),
'MINIMAL':preset('MINIMAL',mark='DOT',max_width=560,padding=20,title_size=34,row_size=20,status_size=13,row_height=56,gap=4,row_radius=0,boot_radius=0,boot_width=132,boot_height=40,boot_align=0),
'HIGH_CONTRAST':preset('HIGH_CONTRAST',max_width=760,title_size=26,row_size=20,status_size=16,row_height=60,gap=8,row_radius=0,boot_radius=0,boot_width=192,boot_height=48,focus_outline=2,press_scale=256),
'FLUENT':preset('FLUENT',mark='BAR',max_width=760,padding=28,title_size=28,row_size=17,status_size=13,row_height=44,gap=0,row_radius=0,boot_radius=0,boot_width=152,boot_height=40,icon_size=20,header=3),
'FLUENT2':preset('FLUENT2',mark='PILL',max_width=760,padding=28,title_size=28,row_size=17,status_size=13,row_height=44,gap=4,panel_radius=12,row_radius=4,boot_radius=4,boot_width=152,boot_height=40,icon_size=20,panel_shadow=12),
'MATERIAL2':preset('MATERIAL2',mark='RADIO',max_width=640,padding=24,title_size=24,row_size=16,status_size=14,row_height=48,gap=0,panel_radius=4,row_radius=0,boot_radius=4,boot_width=112,boot_height=36,icon_size=20,header=2,panel_shadow=8,press_scale=256),
'MATERIAL3':preset('MATERIAL3',mark='RADIO',max_width=640,padding=24,title_size=28,row_size=16,status_size=14,row_height=56,gap=4,panel_radius=28,row_radius=16,boot_radius=255,boot_width=160,boot_height=40,icon_size=24,press_scale=256),
'SURFACE':preset('SURFACE',layout='SPLIT',mark='BAR',max_width=1120,padding=32,title_size=30,row_size=17,status_size=14,row_height=44,gap=0,row_radius=0,boot_radius=2,boot_width=156,boot_height=40,icon_size=20,header=3,press_scale=256),
'CUPERTINO':preset('CUPERTINO',layout='GRID',max_width=920,padding=40,title_size=24,row_size=16,status_size=13,row_height=56,tile_height=160,gap=24,row_radius=10,boot_radius=8,boot_width=140,boot_height=36,boot_align=1,icon_size=72,focus_outline=2,grid_two=460,grid_three=700),
'IOS_HIG':preset('IOS_HIG',layout='INSET',max_width=540,padding=20,title_size=34,row_size=17,status_size=13,row_height=48,gap=0,row_radius=0,boot_radius=255,boot_width=240,boot_height=44,boot_align=1,press_scale=256),
'HARMONYOS':preset('HARMONYOS',layout='GRID',max_width=1040,padding=32,title_size=32,row_size=18,status_size=14,row_height=56,tile_height=156,gap=16,row_radius=24,boot_radius=255,boot_width=220,boot_height=44,boot_align=1,icon_size=48,focus_outline=2),
'CLOVER':preset('CLOVER',layout='RIBBON',mark='CORNERS',max_width=1200,padding=24,title_size=24,row_size=16,status_size=13,row_height=56,tile_height=180,tile_width=176,gap=20,row_radius=0,boot_radius=8,boot_width=180,boot_height=44,boot_align=1,icon_size=96),
'GLASS':preset('GLASS',max_width=680,padding=28,title_size=30,row_height=56,gap=8,panel_radius=24,row_radius=12,boot_radius=12,boot_width=180,panel_shadow=16,background=1),
'AURORA':preset('AURORA',mark='DOT',max_width=720,padding=32,title_size=36,row_height=56,gap=12,panel_radius=24,row_radius=8,boot_radius=255,boot_width=180,background=2),
'SOFT_UI':preset('SOFT_UI',max_width=720,padding=32,title_size=28,row_height=56,gap=16,panel_radius=24,row_radius=16,boot_radius=16,boot_width=184,row_shadow=10),
'BENTO':preset('BENTO',layout='BENTO',max_width=1040,padding=28,title_size=32,row_height=56,tile_height=140,gap=16,row_radius=20,boot_radius=12,boot_width=180,icon_size=32,focus_outline=2),
'NEON':preset('NEON',mark='CORNERS',max_width=840,padding=28,title_size=26,row_size=18,status_size=13,row_height=52,gap=12,row_radius=0,boot_radius=0,boot_width=176,icon_size=20,header=3,focus_glow=8),
'NORD':preset('NORD',mark='BAR',max_width=760,padding=28,title_size=28,row_size=17,status_size=13,row_height=44,gap=4,panel_radius=6,row_radius=2,boot_radius=4,boot_width=160,boot_height=40,icon_size=20),
}
TITLES = {'CLASSIC':'Boot Manager','TERMINAL':'KSHIMC / BOOT SELECTOR','MINIMAL':'Start here','CARDS':'Choose a boot image',
          'FLUENT':'Choose an operating system','FLUENT2':'Start your device','MATERIAL2':'Select system',
          'SURFACE':'kShimC UEFI','CUPERTINO':'Startup Disk','IOS_HIG':'Start up','HARMONYOS':'Boot library',
          'CLOVER':'kShimC Boot Picker','AURORA':'Choose your next start','SOFT_UI':'Start your device',
          'BENTO':'Boot library','NEON':'BOOT / SELECT','NORD':'Boot manager'}
for name, title in TITLES.items():
    DATA[name]['title'] = title
DATA['TERMINAL']['action'] = '[ Boot ]'
for name in ['FLUENT','FLUENT2','MATERIAL2','SURFACE']:
    DATA[name+'_DARK'] = DATA[name].copy()
ORDER = ['CLASSIC','CARDS','TERMINAL','MINIMAL','HIGH_CONTRAST','FLUENT','FLUENT_DARK','FLUENT2','FLUENT2_DARK',
         'MATERIAL2','MATERIAL2_DARK','MATERIAL3','SURFACE','SURFACE_DARK','CUPERTINO','IOS_HIG','HARMONYOS',
         'CLOVER','GLASS','AURORA','SOFT_UI','BENTO','NEON','NORD']

def generate():
    lines = ['/* Generated by tools/generate_design_presets.py; only the selected descriptor is compiled. */',
             '#ifndef KSHIM_MENU_DESIGN_H','#define KSHIM_MENU_DESIGN_H','#include <stdint.h>','#include <stddef.h>']
    for prefix, names in [('KSHIM_DESIGN',KINDS),('KSHIM_LAYOUT',LAYOUTS),('KSHIM_MARK',MARKS)]:
        lines.append('enum { '+', '.join(f'{prefix}_{name}={i+1}' for i,name in enumerate(names))+' };')
    lines += ['typedef struct {','    const char *name, *title, *action;']
    for key in BASE:
        lines.append(f'    uint16_t {key};')
    lines.append('} kshim_menu_design_t;')
    for index, name in enumerate(ORDER):
        lines.append(('#if' if index==0 else '#elif')+f' defined(CONFIG_KSHIM_MENU_STYLE_{name}) && CONFIG_KSHIM_MENU_STYLE_{name}')
        lines.append('static const kshim_menu_design_t mMenuDesign = {')
        for key, value in DATA[name].items():
            if key=='kind': literal='KSHIM_DESIGN_'+value
            elif key=='layout': literal='KSHIM_LAYOUT_'+value
            elif key=='mark': literal='KSHIM_MARK_'+value
            elif isinstance(value,str): literal=json.dumps(value)
            else: literal=str(value)+'U'
            lines.append(f'    .{key} = {literal},')
        lines += [f'    .name = "{name}",','};']
    lines += ['#else','static const kshim_menu_design_t mMenuDesign = {.name="FLAT", .enabled=0U};','#endif','#endif']
    output='\n'.join(lines)+'\n'
    return re.sub(r'\n    (\.[^\n]+)\n    (\.[^\n]+)\n    (\.[^\n]+)',lambda m:'\n    '+' '.join(m.groups()),output)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true')
    args=parser.parse_args()
    path=ROOT/'src/ui/menu_design.h'
    generated=generate()
    if args.check:
        if path.read_text(encoding='utf-8')!=generated:
            raise SystemExit('menu_design.h differs from its generator; regenerate and review the diff')
        print('PASS: 24 selected-only descriptors match their generator')
    else:
        path.write_text(generated,encoding='utf-8')
        print('Generated',path)

if __name__=='__main__':
    main()
