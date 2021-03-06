//
//  fusion-decode.h
//

#ifndef rv_fusion_decode_h
#define rv_fusion_decode_h

namespace riscv {

	struct fusion_decode
	{
		addr_t pc;         /* program counter */
		s64    imm;        /* decoded immediate */
		inst_t inst;       /* source instruction */
		u16    op;         /* (>256 entries) nearly full */
		u8     codec;      /* (>32 entries) can grow */
		u8     rd;         /* (5 bits) byte aligned for performance */
		u8     rs1;        /* (5 bits) byte aligned for performance */
		u8     rs2;        /* (5 bits) byte aligned for performance */
		u8     rs3;        /* (5 bits) byte aligned for performance */
		u8     rm;         /* round mode for some FPU ops */
		u8     aq : 1;     /* acquire for atomic ops */
		u8     rl : 1;     /* release for atomic ops */
		u8     pred : 4;   /* pred for fence */
		u8     succ : 4;   /* succ for fence */

		fusion_decode()
			: pc(0), imm(0), inst(0), op(0), codec(0), rd(0), rs1(0), rs2(0), rs3(0), rm(0), aq(0), rl(0), pred(0), succ(0) {}

		fusion_decode(addr_t pc, uint16_t op, uint8_t rd, int64_t imm)
			: pc(pc), imm(imm), inst(0), op(op), codec(0), rd(rd), rs1(0), rs2(0), rs3(0), rm(0), aq(0), rl(0), pred(0), succ(0) {}
	};

	enum fusion_op {
		fusion_op_li = 1024,
		fusion_op_la = 1025,
		fusion_op_call = 1026
	};

}

#endif
