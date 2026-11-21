"""Compare numeric result bits and accrued flags, one digest per assembled instruction."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

from reference import run_reference
from serial_session import command_output

ROOT = Path(__file__).resolve().parents[1]


def scalar_cases():
    cases = []
    for fmt, width in (("s", 32), ("d", 64)):
        def add(name, operands):
            cases.append((f"{name}.{fmt} {operands}", width, 0))
        for op in ("fadd", "fsub", "fmul", "fdiv", "fmin", "fmax", "fsgnj", "fsgnjn", "fsgnjx"):
            add(op, "f4,f1,f2")
        for op in ("fmadd", "fmsub", "fnmadd", "fnmsub"):
            add(op, "f4,f1,f2,f3")
        add("fsqrt", "f4,f1")
        for op in ("feq", "flt", "fle"):
            add(op, "t2,f1,f2")
        add("fclass", "t2,f1")
        for op in ("fminm", "fmaxm"):
            add(op, "f4,f1,f2")
        for op in ("fleq", "fltq"):
            add(op, "t2,f1,f2")
        for op in ("fround", "froundnx"):
            add(op, "f4,f1")
        for value in ("-1.0","min","0x1p-16","0x1p-15","0x1p-8","0x1p-7","0.0625","0.125",
                      "0.25","0.3125","0.375","0.4375","0.5","0.625","0.75","0.875",
                      "1.0","1.25","1.5","1.75","2.0","2.5","3.0","4.0","8.0","16.0",
                      "128.0","256.0","32768.0","65536.0","inf","nan"):
            add("fli", f"f4,{value}")
        for kind in ("w", "wu", "l", "lu"):
            cases.append((f"fcvt.{kind}.{fmt} t2,f1", width, 0))
            cases.append((f"fcvt.{fmt}.{kind} f4,t1", 0, 0))
    for src, width in (("h", 16), ("s", 32), ("d", 64)):
        for dst in ("h", "s", "d"):
            if src != dst:
                cases.append((f"fcvt.{dst}.{src} f4,f1", width, 0))
        move = "w" if src == "s" else src
        cases.extend([(f"fmv.x.{move} t2,f1", width, 0), (f"fmv.{move}.x f4,t1", 0, 0)])
    cases.append(("fcvtmod.w.d t2,f1,rtz",64,0))
    return cases


def vector_cases():
    # 输出只比较规范定义的有效部分。掩码目的仅比较有效位，归约只比较第零元素。
    cases = []
    def add(asm, width=32, view=0):
        cases.append((asm, width, view))
    def family(names, forms, width=32, view=0, masked=True):
        for name in names.split():
            for form in forms.split():
                arg = {"vv": "v4", "vx": "t1", "vi": "3", "vf": "f1",
                       "wv": "v4", "wx": "t1", "wi": "3", "wf": "f1"}[form]
                # 累加操作数的语法是 vd,vs1,vs2。
                accum = name.endswith(("macc", "madd", "msub", "msac", "maccu", "maccsu", "maccus"))
                operands = f"v16,{arg},v8" if accum else f"v16,v8,{arg}"
                add(f"{name}.{form} {operands}", width, view)
                if masked:
                    add(f"{name}.{form} {operands},v0.t", width, view)
    family("vadd vand vor vxor vsll vsrl vsra vsaddu vsadd vssrl vssra", "vv vx vi")
    family("vsub vminu vmin vmaxu vmax vsaddu vssubu vssub vsmul vaaddu vaadd vasubu vasub "
           "vdivu vdiv vremu vrem vmulhu vmul vmulhsu vmulh vmadd vnmsub vmacc vnmsac", "vv vx", 64)
    family("vrsub", "vx vi")
    family("vandn vrol vror", "vv vx",64)
    for shift in (0,1,31,32,63):
        add(f"vror.vi v16,v8,{shift}",64)
    family("vwsll","vv vx vi",view=3)
    for op in ("vbrev","vbrev8","vrev8","vclz","vctz","vcpop"):
        for w in (8,16,32,64):
            add(f"{op}.v v16,v8,v0.t",w)
    family("vmseq vmsne vmsleu vmsle", "vv vx vi", view=1)
    family("vmslt vmsltu", "vv vx", view=1)
    family("vmsgt vmsgtu", "vx vi", view=1)
    family("vrgather", "vv vx vi")
    family("vrgatherei16", "vv")
    family("vslideup vslidedown", "vx vi")
    family("vslide1up vslide1down", "vx")
    family("vwaddu vwadd vwsubu vwsub vwmulu vwmulsu vwmul vwmaccu vwmacc vwmaccsu",
           "vv vx", view=3)
    family("vwmaccus", "vx", view=3)
    family("vwaddu vwadd vwsubu vwsub", "wv wx", view=3)
    family("vnsrl vnsra vnclipu vnclip", "wv wx wi")
    for op in ("vadc", "vmadc", "vsbc", "vmsbc", "vmerge"):
        for form, arg in (("vvm", "v4"), ("vxm", "t1"), ("vim", "3")):
            if op in ("vsbc","vmsbc") and form == "vim":
                continue
            add(f"{op}.{form} v16,v8,{arg},v0", view=int(op in ("vmadc","vmsbc")))
    for op in ("vredsum","vredmin","vredmax","vredminu","vredmaxu","vredand","vredor","vredxor",
               "vwredsum","vwredsumu","vfredosum","vfredusum","vfredmin","vfredmax","vfwredosum","vfwredusum"):
        add(f"{op}.vs v16,v8,v4,v0.t", view=4 if op.startswith(("vw","vfw")) else 2)
    for op in ("vmandn","vmand","vmor","vmxor","vmorn","vmnand","vmnor","vmxnor"):
        add(f"{op}.mm v16,v8,v4", view=1)
    for op in ("vmsbf","vmsof","vmsif","viota"):
        add(f"{op}.m v16,v8,v0.t", view=int(op != "viota"))
    add("vid.v v16,v0.t")
    for op in ("vcpop","vfirst"):
        add(f"{op}.m t2,v8,v0.t")
    add("vcompress.vm v16,v8,v4", view=5)
    for factor in (2,4,8):
        for op in ("vsext","vzext"):
            add(f"{op}.vf{factor} v16,v8",64)
    for inst in ("vmv.x.s t2,v8","vmv.s.x v16,t1","vmv.v.v v16,v8","vmv.v.x v16,t1","vmv.v.i v16,-3",
                 "vfmv.f.s f4,v8","vfmv.s.f v16,f1","vfmv.v.f v16,f1","vfmerge.vfm v16,v8,f1,v0"):
        add(inst)
    family("vfadd vfsub vfmul vfdiv vfmin vfmax vfsgnj vfsgnjn vfsgnjx vfmadd vfnmadd "
           "vfmsub vfnmsub vfmacc vfnmacc vfmsac vfnmsac", "vv vf")
    family("vfrdiv vfrsub vfslide1up vfslide1down", "vf", 64)
    family("vmfeq vmfne vmflt vmfle", "vv vf", view=1)
    family("vmfgt vmfge", "vf", view=1)
    family("vfwadd vfwsub vfwmul vfwmacc vfwnmacc vfwmsac vfwnmsac", "vv vf", view=3)
    family("vfwadd vfwsub", "wv wf", view=3)
    for op in ("vfsqrt","vfrec7","vfrsqrt7","vfclass"):
        for width in (32,64):
            add(f"{op}.v v16,v8,v0.t", width)
    for mid in ("xu.f","x.f","f.xu","f.x","rtz.xu.f","rtz.x.f","f.f"):
        if mid != "f.f":
            add(f"vfcvt.{mid}.v v16,v8")
        add(f"vfwcvt.{mid}.v v16,v8", view=3)
        add(f"vfncvt.{mid}.w v16,v8")
    add("vfncvt.rod.f.f.w v16,v8")
    add("vfwcvt.f.f.v v16,v8",16,3)
    add("vfncvt.f.f.w v16,v8",16)
    return list(dict.fromkeys(cases))


# a0=指令入口，a1=连续的输入位串，a2=结果缓冲区，a3=vtype，a4=frm，a5=vxrm。
PROBE = """
    .option push
    .option arch, +v, +zfhmin, +zvfhmin, +zfa, +zvbb
    .option norvc
    .text
    .globl probe
probe:
    slli t0,a4,5
    csrw fcsr,t0
    csrw vxrm,a5
    csrwi vxsat,0
    ld t1,0(a1)
    fld f1,0(a1)
    fld f2,8(a1)
    fld f3,16(a1)
    fmv.d.x f4,zero
    li t2,0
    addi t0,a1,32
    vl2re8.v v4,(t0)
    addi t0,a1,64
    vl2re8.v v8,(t0)
    addi t0,a1,96
    vl2re8.v v16,(t0)
    vl1re8.v v0,(a1)
    li t0,2
    vsetvl t3,t0,a3
    jalr t6,0(a0)
    fsd f4,0(a2)
    sd t2,8(a2)
    frflags t0
    sd t0,16(a2)
    csrr t0,vxsat
    sd t0,24(a2)
    addi a2,a2,32
    vs2r.v v16,(a2)
    ret
    .balign 4
    .globl sites
sites:
"""

DRIVER = """
#include "user/api.h"
typedef uint64_t U;
extern const char sites[];
void probe(const char *,const U *,U *,unsigned,unsigned,unsigned);
static U random_word(U *s) {
    *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s;
}
static U input(unsigned width, unsigned n, U *seed) {
    unsigned frac=width==16?10:width==32?23:52, exp=width==16?5:width==32?8:11;
    U mask=UINT64_MAX>>(64-width), sign=(U)1<<(width-1), unit=(U)1<<frac;
    U inf=sign-unit, one=((U)1<<(exp-1))*unit-unit;
    U cases[]={0,sign,1,unit-1,unit,one,one+1,one-1,inf-1,inf,inf+1,inf+unit/2,sign|inf,sign|one};
    U x=n<14?cases[n]:random_word(seed);
    return (x & mask) | ~mask;
}
void program_entry(void) {
    static const unsigned char widths[]={WIDTHS};
    static const unsigned char views[]={VIEWS};
    static const char digits[]="0123456789abcdef";
    for (unsigned op=0;op<sizeof(widths);++op) {
        U h=0xcbf29ce484222325ULL, seed=0x123456789abcdefULL;
        unsigned width=widths[op]?widths[op]:64, w=width/8;
        for (unsigned k=0;k<SAMPLES;++k) {
            U in[16],out[8];
            for (unsigned j=0;j<16;++j) in[j]=random_word(&seed);
            if (!VECTOR) {
                in[0]=input(width,k%24,&seed);
                in[1]=input(width,(k*7+k/24)%24,&seed);
                in[2]=input(width,(k*11+k/24)%24,&seed);
            } else if (k & 1) {
                for (unsigned j=0;j<16;++j) {
                    U a=input(width,(k+j)%20,&seed), b=input(width,(k+2*j+1)%20,&seed);
                    in[j]=width==64?a:(a<<32)|(uint32_t)b;
                }
            }
            probe(sites+8*op,in,out,width==64?24:width==32?16:width==16?8:0,k%5,k%4);
            unsigned bytes=32;
            if (VECTOR) {
                switch (views[op]) {
                case 1: out[4]&=3; bytes+=1; break;
                case 2: bytes+=w; break;
                case 3: bytes+=4*w; break;
                case 4: bytes+=2*w; break;
                case 5: bytes+=w*((in[4]&1)+((in[4]>>1)&1)); break;
                default: bytes+=2*w;
                }
            }
            for (unsigned j=0;j<bytes;++j) h=(h^((unsigned char*)out)[j])*0x100000001b3ULL;
        }
        char line[17];
        for (unsigned j=0;j<16;++j) line[15-j]=digits[(h>>(4*j))&15];
        line[16]='\\n'; user_write(line,17);
    }
    user_exit(0);
}
"""


def build(temp, name, cases):
    c = temp / f"{name}.c"
    s = temp / f"{name}.S"
    elf = temp / name
    c.write_text(DRIVER.replace("WIDTHS", ",".join(str(x[1]) for x in cases))
                 .replace("VIEWS", ",".join(str(x[2]) for x in cases))
                 .replace("VECTOR", str(int(name == "vector")))
                 .replace("SAMPLES", "20" if name == "vector" else "84"), encoding="utf-8")
    s.write_text(PROBE + "".join(f"    {asm}\n    jr t6\n" for asm, _, _ in cases) + ".option pop\n")
    subprocess.run(["clang", "--target=riscv64-unknown-elf", "-march=rv64imac", "-mabi=lp64",
                    "-std=c23", "-Os", "-ffreestanding", "-nostdlib", "-mno-relax", "-mcmodel=medany",
                    "-msmall-data-limit=0", f"-I{ROOT/'OS'}", "-fuse-ld=lld",
                    f"-Wl,-T,{ROOT/'OS/user/disk.ld'}", "-Wl,--defsym=APP_BASE=0x40010000",
                    str(c), str(s), str(ROOT/"OS/user/syscall.S"), "-o", str(elf)], check=True)
    return elf


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("vm", "qemu", "images"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    groups = {"scalar": scalar_cases(), "vector": vector_cases()}
    with tempfile.TemporaryDirectory() as path:
        temp = Path(path)
        disk = temp / "numeric.img"
        pack = [sys.executable, ROOT/"tools/mkdisk.py", args.images/"kernel.bin", disk]
        for name, cases in groups.items():
            pack.extend(["--file", f"{name}={build(temp,name,cases)}"])
        subprocess.run(pack, check=True)
        fw = args.images/"firmware.bin"
        commands = b"run scalar\nrun vector\nhalt\n"
        vm = subprocess.run([args.vm,"--firmware",fw,"--disk",disk,"--max-steps","90000000"],
                            input=commands,capture_output=True,timeout=70,check=True)
        reference = run_reference(args.qemu,fw,disk,commands,vlen=128)
        for name,cases in groups.items():
            def records(serial):
                body = command_output(serial.replace(b"\r\n",b"\n"),f"run {name}".encode())
                rows = re.findall(rb"^([0-9a-f]{16})$",body,re.M)
                if len(rows) != len(cases) or not body.endswith(b"user exit: 0\n"):
                    index = min(len(rows),len(cases)-1)
                    raise AssertionError(f"{name} stopped near {cases[index][0]}: {body[-180:]!r}")
                return rows
            native, other = records(vm.stdout), records(reference)
            diffs = [f"{case[0]}: {a.decode()} / {b.decode()}" for case,a,b in zip(cases,native,other) if a!=b]
            if diffs: raise AssertionError("\n".join(diffs))
            print(f"{name}: {len(cases)} instruction forms matched")
        for vlen in (256, 512):
            serial = run_reference(args.qemu, fw, args.images / "disk.img",
                                   b"run /numeric.elf\nhalt\n", vlen=vlen)
            body = command_output(serial.replace(b"\r\n", b"\n"), b"run /numeric.elf")
            if not body.endswith(b"numeric: fork, timer and wait preserved registers\nuser exit: 0\n"):
                raise AssertionError(f"VLEN={vlen}: {body!r}")
            print(f"VLEN={vlen}: task context and cross-page accesses passed")


if __name__ == "__main__":
    main()
