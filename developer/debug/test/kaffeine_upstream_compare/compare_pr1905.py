#!/usr/bin/env python3
"""Reproduce the pinned PR #1905 source-function comparison with Kaffeine.

This is an offline, host-only comparison receipt, not an AROS acceptance
suite. Successful completion means that the documented reference behaviors
were reproduced, including failures in the original upstream revision.
Requires Python 3.9+, a POSIX host and a C++ compiler with ASan/UBSan.
"""
# Copyright (C) 2026, The AROS Development Team. All rights reserved.
# Author: Michele Dipace (@kaffeine1)
# Source functions retain their original AROS provenance. See README.md.

from pathlib import Path
import argparse, hashlib, json, os, re, shlex, subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--upstream-root', type=Path, required=True,
                    help='Local AROS source tree at the reference PR revision')
parser.add_argument('--kaffeine-root', type=Path,
                    default=Path(__file__).resolve().parents[4])
parser.add_argument('--build-dir', type=Path, required=True,
                    help='New or empty output directory outside the source tree')
parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
args = parser.parse_args()
HERE = args.build_dir.resolve()
OURS = args.kaffeine_root.resolve()
upstream_root = args.upstream_root.resolve()
UP = upstream_root / 'rom/m68kemu'
REFERENCE_SHA256 = {'rom/m68kemu/m68kemu_intern.h': '3f6e15a6ca1936a2716f4ed0fa2169489a80430c926769d991045473adf24313', 'rom/m68kemu/m68kemu_memory.c': 'ab678dead35936c4851843a20261c83b8efe3c3ef772fbe2baccbbefca07739b', 'rom/m68kemu/m68kemu_moira.cpp': '142c419da9e8891623c8726c9a545cfb8f335f6a737624d46d3485d487c500a7', 'rom/m68kemu/m68kemu_offsets.h': 'c2d0d64f5da5adf4fa33a8b3cf688dd3b130ee2003dd986b9f79877873d95b7f', 'rom/m68kemu/m68kemu_thunks.c': '6cafd2733820a79eb0f2f79668a5f23e03a7ad19e2fc474c754daeeb14a319c7'}
for relative, expected in REFERENCE_SHA256.items():
    path = upstream_root / relative
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        parser.error(f'Upstream input differs from reference PR #1905: {relative}')
if not (OURS / 'rom/kaffeine/kaffeine_memheader.c').is_file():
    parser.error('Kaffeine source tree not found')
if HERE.exists() and any(HERE.iterdir()):
    parser.error('--build-dir must be new or empty')
OUT = HERE / 'bin'
OUT.mkdir(parents=True)


def function(path, name):
    source = path.read_text()
    # Ignore braces inside strings and comments without changing positions.
    mask = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source)
    pattern = r'(?m)^[^\n;{}]*\b' + re.escape(name) + r'\s*\([^;{}]*\)[^;{}]*\{'
    match = re.search(pattern, mask)
    if not match:
        raise RuntimeError(f'Cannot find {name} in {path}')
    begin = mask.index('{', match.start())
    depth = 1
    end = begin + 1
    while depth:
        depth += (mask[end] == '{') - (mask[end] == '}')
        end += 1
    text = source[match.start():end]
    extracts.append({'file': ('upstream/' + str(path.relative_to(upstream_root))) if path.is_relative_to(upstream_root) else ('kaffeine/' + str(path.relative_to(OURS))), 'function': name,
                     'line': source[:match.start()].count('\n') + 1,
                     'sha256': hashlib.sha256(text.encode()).hexdigest()})
    return text

extracts = []
prefix = r'''
/* AROS source-function comparison fixture. Copyright (C) 2026, AROS Development Team. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using UBYTE=uint8_t; using UWORD=uint16_t; using ULONG=uint32_t;
using LONG=int32_t; using IPTR=uintptr_t; using BOOL=int; using STRPTR=char*;
#define TRUE 1
#define FALSE 0
#define M68KEmuContext KaffeineContext
#define bug(...) ((void)0)
#define RGN_CHIP 0
#define RGN_FAST 1
#define M68KEMU_CHIP_END 0x200000u
#define M68KEMU_ROM_BASE 0xF80000u
#define M68KEMU_ROM_END 0x1000000u
#define M68KEMU_ROM_SIZE 0x80000u
#define M68KEMU_HEAP_HDR 8u
#define M68KEMU_HEAP_MIN_SPLIT 16u
#define M68K_MH_FIRST 16
#define M68K_MH_FREE 28
#define M68K_LIB_VERSION 20
#define MEMF_CHIP 2u
#define MEMF_FAST 4u
#define MEMF_CLEAR 0x10000u
#define MEMF_LARGEST 0x20000u
#define MEMF_TOTAL 0x80000u
#define FB_NEXT 0u
#define FB_SIZE 4u
#define KAFFEINE_MEMINFO_SIZE 96u
struct M68KHeapRegion { ULONG start,end,free,avail,mh; };
struct KaffeineContext {
 UBYTE *mem; ULONG mem_size; int low_guard=0,trace_calls=0,running=1;
 ULONG heap_start=0,heap_end=0,memory_info=0; UWORD num_shadows=0;
 struct { ULONG m68k_addr; void *native_ptr; } shadow_map[1]{};
 M68KHeapRegion rgn[2]{};
 struct { char name[64]; ULONG base,reopen; } m68klibs[1]{};
 UWORD num_m68klibs=0;
 ULONG tail_pc=0,tail_d0=0,tail_a0=0,tail_a6=0;
};
struct CPU { ULONG d[8]{},a[8]{}; };
#define THUNK_D(n) (((CPU*)cpu)->d[n])
#define THUNK_A(n) (((CPU*)cpu)->a[n])
#define THUNK_PTR(n) ((void*)(ctx->mem+THUNK_A(n)))
static void m68k_low_write_trap(KaffeineContext*,ULONG,const char*) {}
'''
tail = r'''
static void put(KaffeineContext &c,ULONG p,ULONG v) {
 c.mem[p]=v>>24;c.mem[p+1]=v>>16;c.mem[p+2]=v>>8;c.mem[p+3]=v;
}
'''
access_main = r'''
int main(int argc,char**argv) {
 std::vector<UBYTE> mem(17,0xA5); KaffeineContext c{};c.mem=mem.data();c.mem_size=mem.size();
 const char *which=argv[1];
 if(!strcmp(which,"control")) {
  if(m68k_read16(&c,15)!=0xA5A5 || m68k_read32(&c,13)!=0xA5A5A5A5u) return 1;
  auto before=mem;m68k_write32(&c,14,0);if(mem!=before || m68k_read16(&c,16)!=0) return 2;
 } else if(!strcmp(which,"read16-wrap")) {if(m68k_read16(&c,0xFFFFFFFFu)!=0)return 3;}
 else if(!strcmp(which,"read32-wrap")) {if(m68k_read32(&c,0xFFFFFFFEu)!=0)return 3;}
 else if(!strcmp(which,"write16-wrap")) m68k_write16(&c,0xFFFFFFFFu,0);
 else if(!strcmp(which,"write32-wrap")) m68k_write32(&c,0xFFFFFFFEu,0);
 else return 99;
 puts("contract satisfied");return 0;
}
'''
mem_main = r'''
int main(int argc,char**argv) {
 std::vector<UBYTE> mem(4096,0); KaffeineContext c{};c.mem=mem.data();c.mem_size=mem.size();
 CPU cpu{}; const ULONG header=0x100,pool=0x200;
 put(c,header+16,pool);put(c,header+20,pool);put(c,header+24,pool+128);put(c,header+28,128);
 put(c,pool,0);put(c,pool+4,128);
 cpu.a[0]=header;cpu.d[0]=8;
 const char *which=argv[1];
 if(!strcmp(which,"control")) {
  auto address=ALLOC(&c,&cpu);if(address!=pool || m68k_read32(&c,header+28)!=120)return 1;
  cpu.a[1]=address;FREE(&c,&cpu);if(m68k_read32(&c,header+28)!=128)return 2;
 } else {
  bool release=false;
  if(!strcmp(which,"overflow"))cpu.d[0]=0xFFFFFFFFu;
  else if(!strcmp(which,"bad-successor"))put(c,pool,pool);
  else if(!strcmp(which,"overlap-free")){release=true;cpu.a[1]=pool+16;cpu.d[0]=8;}
  else if(!strcmp(which,"outside-pool")){release=true;cpu.a[1]=0x400;cpu.d[0]=8;}
  else return 99;
  auto before=mem;
  IPTR answer=release?FREE(&c,&cpu):ALLOC(&c,&cpu);
  if(answer!=0 || mem!=before) {
   printf("invalid operation accepted: result=%lu, bytes_changed=%d, free=%u\n",(unsigned long)answer,mem!=before,m68k_read32(&c,header+28));
   return 3;
  }
 }
 puts("contract satisfied");return 0;
}
'''

programs = {}
for variant, header in [('upstream',UP/'m68kemu_intern.h'),('kaffeine',OURS/'rom/kaffeine/kaffeine_intern.h')]:
    helpers = '\n'.join(function(header,n) for n in ['m68k_read16','m68k_read32','m68k_write16','m68k_write32'])
    programs[variant+'-access'] = prefix+helpers+access_main
    if variant=='upstream':
        bodies='\n'.join(function(UP/'m68kemu_thunks.c',n) for n in ['thunk_exec_Allocate_m','thunk_exec_Deallocate_m'])
        bodies='#define M68K_MEM_BLOCKSIZE 8\n'+bodies+'\n#define ALLOC thunk_exec_Allocate_m\n#define FREE thunk_exec_Deallocate_m\n'
    else:
        bodies='#define KAFFEINE_INTERN_H\n#define KAFFEINE_MEMHEADER_H\n#include "'+str(OURS/'rom/kaffeine/kaffeine_memheader.c')+'"\n'
        bodies+='\n#define ALLOC Kaffeine_MemHeaderAllocate\n#define FREE Kaffeine_MemHeaderDeallocate\n'
    programs[variant+'-memheader']=prefix+helpers+tail+bodies+mem_main

helpers='\n'.join(function(UP/'m68kemu_intern.h',n) for n in ['m68k_read16','m68k_read32','m68k_write16','m68k_write32'])
heap='\n'.join(function(UP/'m68kemu_memory.c',n) for n in ['heap_publish','M68KEmu_HeapRegion','heap_largest','heap_alloc_in','M68KEmu_HeapAlloc'])
programs['upstream-heap']=prefix+helpers+tail+heap+r'''
int main(int argc,char**argv) {
 if(!strcmp(argv[1],"custom-hole")) {
  std::vector<UBYTE> mem(32*1024*1024,0);KaffeineContext c{};c.mem=mem.data();c.mem_size=mem.size();
  c.rgn[RGN_FAST]={0x200000,0x1FF0000,0x200000,0x1D70000,0};
  put(c,0x200000,0x1000000);put(c,0x200004,0xD80000);
  put(c,0x1000000,0);put(c,0x1000004,0xFF0000);
  ULONG result=M68KEmu_HeapAlloc(&c,0xC00000,MEMF_FAST);
  bool overlaps=result&&result<=0xDFF000&&result+0xC00000>0xDFF000;
  printf("12 MiB Fast allocation: %08x, includes custom register page=%d\n",result,overlaps);
  return result&&!overlaps?0:3;
 }
 std::vector<UBYTE> mem(4096,0); KaffeineContext c{};c.mem=mem.data();c.mem_size=mem.size();
 c.rgn[RGN_FAST]={0x200,0x600,0x200,0x400,0};put(c,0x200,0);put(c,0x204,0x400);
 if(!strcmp(argv[1],"control"))return M68KEmu_HeapAlloc(&c,16,0)==0x5F0?0:1;
 auto before=mem;ULONG result=M68KEmu_HeapAlloc(&c,0xFFFFFFFFu,0);
 printf("HeapAlloc(UINT32_MAX): %08x, free=%u, changed=%d\n",result,c.rgn[RGN_FAST].avail,mem!=before);
 return result==0&&mem==before?0:3;
}
'''
opener='\n'.join(function(UP/'m68kemu_thunks.c',n) for n in ['thunk_exec_OpenLibrary','thunk_exec_OldOpenLibrary'])
programs['upstream-library']=prefix+helpers+tail+r'''
static ULONG M68KEmu_OpenLibrary(KaffeineContext*,const char*){return 0;}
static ULONG m68k_open_class_library(KaffeineContext*,const char*,ULONG){return 0;}
static ULONG M68KEmu_LoadM68KLibrary(KaffeineContext*,const char*,ULONG){return 0;}
static char *FilePart(char *p){return p;}
'''+opener+r'''
int main(int argc,char**argv) {
 std::vector<UBYTE> mem(4096,0); KaffeineContext c{};c.mem=mem.data();c.mem_size=mem.size();
 c.num_m68klibs=1;strcpy(c.m68klibs[0].name,"tiny.library");c.m68klibs[0].base=0x100;c.m68klibs[0].reopen=0x300;
 put(c,0x100+20,0x00020000);strcpy((char*)c.mem+0x200,"tiny.library");
 CPU cpu{};cpu.a[1]=0x200;cpu.d[0]=!strcmp(argv[1],"control")?2:0x12340002;
 auto result=!strcmp(argv[1],"old-open")?thunk_exec_OldOpenLibrary(&c,&cpu):thunk_exec_OpenLibrary(&c,&cpu);
 printf("cached OpenLibrary version %08x: result=%08lx\n",cpu.d[0],(unsigned long)result);
 return result==0x100&&c.tail_pc==0x300?0:3;
}
'''

# Exercise the upstream read callback verbatim; customRead16 only supplies
# POTGOR's documented value. This is not a full Moira/QEMU execution test.
read16=function(UP/'m68kemu_moira.cpp','read16')
offsets=(UP/'m68kemu_offsets.h').read_text()
mask=re.search(r'^#define CUSTOM_MASK\s+(\S+)',offsets,re.M)[1]
base=re.search(r'^#define CUSTOM_BASE\s+(\S+)',offsets,re.M)[1]
programs['upstream-alias']=prefix+r'''
using u16=uint16_t;using u32=uint32_t;
#define NEST_SENTINEL 0xFFDEAE00u
#define SENTINEL_ADDR 0xFFDEAD00u
#define M68K_OP_STOP 0x4e72
'''+f'\n#define CUSTOM_MASK {mask}\n#define CUSTOM_BASE {base}\n'+r'''
struct ReadBase {virtual u16 read16(u32)const=0;};
struct AROSMoira:ReadBase {
 KaffeineContext *ctx;bool nested=false,nested_done=false;
 u16 customRead16(u16 reg)const{return reg==0x16?0xFF00:0;}
'''+read16+r'''
};
int main(int argc,char**argv){
 std::vector<UBYTE> mem(32*1024*1024,0xA5);KaffeineContext c{};c.mem=mem.data();c.mem_size=mem.size();AROSMoira cpu;cpu.ctx=&c;
 if(!strcmp(argv[1],"control"))return cpu.read16(0x00DFF016)==0xFF00?0:1;
 auto value=cpu.read16(0x01DFF016);printf("read16($1DFF016)=%04x, RAM=a5a5\n",value);return value==0xA5A5?0:3;
}
'''

for name,source in programs.items():
    cpp=OUT/(name+'.cpp');cpp.write_text(source)
    command=shlex.split(args.cxx)+['-std=c++17','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all',str(cpp),'-o',str(OUT/name)]
    result=subprocess.run(command,capture_output=True,text=True)
    (OUT/(name+'-build.local.log')).write_text(result.stdout+result.stderr)
    if result.returncode: raise RuntimeError(f'Build failed: {name}: '+result.stderr[-2000:])

results=[]
def run(name,case,expected):
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0:abort_on_error=1')
    try:
        p=subprocess.run([str(OUT/name),case],capture_output=True,text=True,timeout=5,env=env)
        row={'program':name,'case':case,'returncode':p.returncode,'contract_passed':p.returncode==0,'stdout':p.stdout,'stderr_first_line':p.stderr.splitlines()[:1]}
        (OUT/(name+'-'+case+'.local.log')).write_text(p.stdout+p.stderr)
    except subprocess.TimeoutExpired:
        row={'program':name,'case':case,'timed_out':True,'contract_passed':False}
    row['expected_pass']=expected
    assert row['contract_passed']==expected,row
    results.append(row)
    print(name,case,'PASS' if row['contract_passed'] else 'REPRODUCED FAILURE',flush=True)

for variant in ['upstream','kaffeine']:
    for case in ['control','read16-wrap','read32-wrap','write16-wrap','write32-wrap']:
        run(variant+'-access',case,variant=='kaffeine' or case=='control')
    for case in ['control','overflow','bad-successor','overlap-free','outside-pool']:
        run(variant+'-memheader',case,variant=='kaffeine' or case=='control')
run('upstream-heap','control',True)
run('upstream-heap','overflow',False)
run('upstream-heap','custom-hole',False)
run('upstream-library','control',True)
run('upstream-library','high-word',False)
run('upstream-library','old-open',False)
run('upstream-alias','control',True)
run('upstream-alias','alias',False)
(HERE/'probe-results.json').write_text(json.dumps({'extractions':extracts,'results':results,'scope':'Host function-level probes with fixture context/native collaborators; no upstream native build or VM run; ASan/UBSan, leak detection disabled'},indent=2)+'\n')
print('All expected comparative observations reproduced.')
