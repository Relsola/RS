#include "cpu_compressed.h"

#include <cstdio>

struct Example
{
    uint16_t compressed;
    uint32_t expanded;
};

// 两列分别由 Clang 汇编压缩指令和它的 32 位展开式得到。
static constexpr Example EXAMPLES[] = {
    {0x0e74, 0x31c10693}, // c.addi4spn a3, sp, 796
    {0x57f0, 0x06c7a603}, // c.lw a2, 108(a5)
    {0x6c6c, 0x0d843583}, // c.ld a1, 216(s0)
    {0xcefc, 0x04f6ae23}, // c.sw a5, 92(a3)
    {0xf764, 0x0e973423}, // c.sd s1, 232(a4)
    {0x0001, 0x00000013}, // c.nop
    {0x12b5, 0xfed28293}, // c.addi t0, -19
    {0x2701, 0x0007071b}, // c.addiw a4, 0
    {0x53a5, 0xfe900393}, // c.li t2, -23
    {0x7595, 0xfffe55b7}, // c.lui a1, 0xfffe5
    {0x7169, 0xed010113}, // c.addi16sp sp, -304
    {0x90b5, 0x02d4d493}, // c.srli s1, 45
    {0x9505, 0x42155513}, // c.srai a0, 33
    {0x9989, 0xfe25f593}, // c.andi a1, -30
    {0x8c8d, 0x40b484b3}, // c.sub s1, a1
    {0x8e39, 0x00e64633}, // c.xor a2, a4
    {0x8edd, 0x00f6e6b3}, // c.or a3, a5
    {0x8f61, 0x00877733}, // c.and a4, s0
    {0x9c05, 0x4094043b}, // c.subw s0, s1
    {0x9fa9, 0x00a787bb}, // c.addw a5, a0
    {0xbc0d, 0xa33ff06f}, // c.j -1486
    {0xc1e9, 0x0c058163}, // c.beqz a1, 194
    {0xf329, 0xf40711e3}, // c.bnez a4, -190
    {0x151a, 0x02651513}, // c.slli a0, 38
    {0x42fe, 0x0dc12283}, // c.lwsp t0, 220(sp)
    {0x6ff6, 0x15813f83}, // c.ldsp t6, 344(sp)
    {0x8382, 0x00038067}, // c.jr t2
    {0x85ce, 0x013005b3}, // c.mv a1, s3
    {0x9002, 0x00100073}, // c.ebreak
    {0x9682, 0x000680e7}, // c.jalr a3
    {0x934a, 0x01230333}, // c.add t1, s2
    {0xcbd6, 0x0d512a23}, // c.swsp s5, 212(sp)
    {0xe7ae, 0x1cb13423}, // c.sdsp a1, 456(sp)
    {0x8000, 0x00044403}, // c.lbu s0, 0(s0)
    {0x81c8, 0x0015c503}, // c.lbu a0, 1(a1)
    {0x81a8, 0x0025c503}, // c.lbu a0, 2(a1)
    {0x81e8, 0x0035c503}, // c.lbu a0, 3(a1)
    {0x85a8, 0x0025d503}, // c.lhu a0, 2(a1)
    {0x85e8, 0x00259503}, // c.lh a0, 2(a1)
    {0x89c8, 0x00a580a3}, // c.sb a0, 1(a1)
    {0x89a8, 0x00a58123}, // c.sb a0, 2(a1)
    {0x8da8, 0x00a59123}, // c.sh a0, 2(a1)
    {0x9d61, 0x0ff57513}, // c.zext.b a0
    {0x9ce5, 0x60449493}, // c.sext.b s1
    {0x9fe9, 0x0807c7bb}, // c.zext.h a5
    {0x9ded, 0x60559593}, // c.sext.h a1
    {0x9f71, 0x0807073b}, // c.zext.w a4
    {0x9c75, 0xfff44413}, // c.not s0
    {0x9d51, 0x02c50533}, // c.mul a0, a2
    // HINT 合法退休，展开计算时写 x0 或保持寄存器的旧值。
    {0x0005, 0x00100013}, // c.nop with imm=1
    {0x0281, 0x00028293}, // c.addi t0, 0
    {0x4005, 0x00100013}, // c.li zero, 1
    {0x6005, 0x00001037}, // c.lui zero, 1
    {0x0006, 0x00101013}, // c.slli zero, 1
    {0x0282, 0x00029293}, // c.slli t0, 0
    {0x8001, 0x00045413}, // c.srli s0, 0
    {0x8401, 0x40045413}, // c.srai s0, 0
    {0x800a, 0x00200033}, // c.mv zero, sp
    {0x900a, 0x00200033}, // c.add zero, sp
};

int main()
{
    for (Example example : EXAMPLES) {
        uint32_t instruction = 0;
        if (!cpu_expand_compressed(example.compressed, &instruction) || instruction != example.expanded) {
            std::fprintf(stderr, "0x%04x expanded to 0x%08x, expected 0x%08x\n",
                         unsigned(example.compressed), instruction, example.expanded);
            return 1;
        }
    }
    static constexpr uint16_t ILLEGAL[] = {
        0x0000, 0x001c, // C.ADDI4SPN with zero immediate
        0x2001,         // C.ADDIW rd=x0
        0x6001, 0x6101, 0x6281, // C.LUI / C.ADDI16SP with zero immediate
        0x4002, 0x6002, // C.LWSP / C.LDSP rd=x0
        0x8002,         // C.JR x0
        0x9c79, 0x9c7d, // 两个未分配的 Zcb 单操作数编号
        0x8c40, 0x9000, 0x9400, 0x9800, 0x9c00, // C.SH 的 bit 6、未分配的访存编码
        0x2000, 0xa000, 0x2002, 0xa002, // compressed floating-point instructions
        0xffff,         // not a compressed encoding
    };
    for (uint16_t code : ILLEGAL) {
        uint32_t instruction = 0;
        if (cpu_expand_compressed(code, &instruction)) {
            std::fprintf(stderr, "unexpected expansion of 0x%04x\n", unsigned(code));
            return 1;
        }
    }
    return 0;
}
