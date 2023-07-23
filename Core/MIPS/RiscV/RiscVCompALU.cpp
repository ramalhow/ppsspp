// Copyright (c) 2023- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "Common/CPUDetect.h"
#include "Core/MemMap.h"
#include "Core/MIPS/RiscV/RiscVJit.h"
#include "Core/MIPS/RiscV/RiscVRegCache.h"

// This file contains compilation for integer / arithmetic / logic related instructions.
//
// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.  No flags because that's in IR already.

// #define CONDITIONAL_DISABLE { CompIR_Generic(inst); return; }
#define CONDITIONAL_DISABLE {}
#define DISABLE { CompIR_Generic(inst); return; }
#define INVALIDOP { _assert_msg_(false, "Invalid IR inst %d", (int)inst.op); CompIR_Generic(inst); return; }

namespace MIPSComp {

using namespace RiscVGen;
using namespace RiscVJitConstants;

void RiscVJit::CompIR_Arith(IRInst inst) {
	CONDITIONAL_DISABLE;

	bool allowPtrMath = true;
#ifndef MASKED_PSP_MEMORY
	// Since we modify it, we can't safely.
	allowPtrMath = false;
#endif

	// RISC-V only adds signed immediates, so rewrite a small enough subtract to an add.
	// We use -2047 and 2048 here because the range swaps.
	if (inst.op == IROp::SubConst && (int32_t)inst.constant >= -2047 && (int32_t)inst.constant <= 2048) {
		inst.op = IROp::AddConst;
		inst.constant = (uint32_t)-(int32_t)inst.constant;
	}

	switch (inst.op) {
	case IROp::Add:
		gpr.MapDirtyInIn(inst.dest, inst.src1, inst.src2, MapType::AVOID_LOAD_MARK_NORM32);
		ADDW(gpr.R(inst.dest), gpr.R(inst.src1), gpr.R(inst.src2));
		break;

	case IROp::Sub:
		gpr.MapDirtyInIn(inst.dest, inst.src1, inst.src2, MapType::AVOID_LOAD_MARK_NORM32);
		SUBW(gpr.R(inst.dest), gpr.R(inst.src1), gpr.R(inst.src2));
		break;

	case IROp::AddConst:
		if ((int32_t)inst.constant >= -2048 && (int32_t)inst.constant <= 2047) {
			// Typical of stack pointer updates.
			if (gpr.IsMappedAsPointer(inst.src1) && inst.dest == inst.src1 && allowPtrMath) {
				gpr.MarkPtrDirty(gpr.RPtr(inst.dest));
				ADDI(gpr.RPtr(inst.dest), gpr.RPtr(inst.dest), inst.constant);
			} else {
				gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
				ADDIW(gpr.R(inst.dest), gpr.R(inst.src1), inst.constant);
			}
		} else {
			gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
			LI(SCRATCH1, (int32_t)inst.constant);
			ADDW(gpr.R(inst.dest), gpr.R(inst.src1), SCRATCH1);
		}
		break;

	case IROp::SubConst:
		gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
		LI(SCRATCH1, (int32_t)inst.constant);
		SUBW(gpr.R(inst.dest), gpr.R(inst.src1), SCRATCH1);
		break;

	case IROp::Neg:
		gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
		SUBW(gpr.R(inst.dest), R_ZERO, gpr.R(inst.src1));
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Logic(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::And:
		gpr.MapDirtyInIn(inst.dest, inst.src1, inst.src2);
		AND(gpr.R(inst.dest), gpr.R(inst.src1), gpr.R(inst.src2));
		break;

	case IROp::Or:
		gpr.MapDirtyInIn(inst.dest, inst.src1, inst.src2);
		OR(gpr.R(inst.dest), gpr.R(inst.src1), gpr.R(inst.src2));
		// If both were normalized before, the result is normalized.
		if (gpr.IsNormalized32(inst.src1) && gpr.IsNormalized32(inst.src2))
			gpr.MarkDirty(gpr.R(inst.dest), true);
		break;

	case IROp::Xor:
		gpr.MapDirtyInIn(inst.dest, inst.src1, inst.src2);
		XOR(gpr.R(inst.dest), gpr.R(inst.src1), gpr.R(inst.src2));
		break;

	case IROp::AndConst:
		if ((int32_t)inst.constant >= -2048 && (int32_t)inst.constant <= 2047) {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			ANDI(gpr.R(inst.dest), gpr.R(inst.src1), inst.constant);
		} else {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			LI(SCRATCH1, (int32_t)inst.constant);
			AND(gpr.R(inst.dest), gpr.R(inst.src1), SCRATCH1);
		}
		// If the sign bits aren't cleared, and it was normalized before - it still is.
		if ((inst.constant & 0x80000000) != 0 && gpr.IsNormalized32(inst.src1))
			gpr.MarkDirty(gpr.R(inst.dest), true);
		// Otherwise, if we cleared the sign bits, it's naturally normalized.
		else if ((inst.constant & 0x80000000) == 0)
			gpr.MarkDirty(gpr.R(inst.dest), true);
		break;

	case IROp::OrConst:
		if ((int32_t)inst.constant >= -2048 && (int32_t)inst.constant <= 2047) {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			ORI(gpr.R(inst.dest), gpr.R(inst.src1), inst.constant);
		} else {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			LI(SCRATCH1, (int32_t)inst.constant);
			OR(gpr.R(inst.dest), gpr.R(inst.src1), SCRATCH1);
		}
		// Since our constant is normalized, oring its bits in won't hurt normalization.
		if (gpr.IsNormalized32(inst.src1))
			gpr.MarkDirty(gpr.R(inst.dest), true);
		break;

	case IROp::XorConst:
		if ((int32_t)inst.constant >= -2048 && (int32_t)inst.constant <= 2047) {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			XORI(gpr.R(inst.dest), gpr.R(inst.src1), inst.constant);
		} else {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			LI(SCRATCH1, (int32_t)inst.constant);
			XOR(gpr.R(inst.dest), gpr.R(inst.src1), SCRATCH1);
		}
		break;

	case IROp::Not:
		gpr.MapDirtyIn(inst.dest, inst.src1);
		NOT(gpr.R(inst.dest), gpr.R(inst.src1));
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Assign(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Mov:
		gpr.MapDirtyIn(inst.dest, inst.src1);
		MV(gpr.R(inst.dest), gpr.R(inst.src1));
		gpr.MarkDirty(gpr.R(inst.dest), gpr.IsNormalized32(inst.src1));
		break;

	case IROp::Ext8to32:
		if (cpu_info.RiscV_Zbb) {
			gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
			SEXT_B(gpr.R(inst.dest), gpr.R(inst.src1));
		} else {
			gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
			SLLI(gpr.R(inst.dest), gpr.R(inst.src1), 24);
			SRAIW(gpr.R(inst.dest), gpr.R(inst.dest), 24);
		}
		break;

	case IROp::Ext16to32:
		if (cpu_info.RiscV_Zbb) {
			gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
			SEXT_H(gpr.R(inst.dest), gpr.R(inst.src1));
		} else {
			gpr.MapDirtyIn(inst.dest, inst.src1, MapType::AVOID_LOAD_MARK_NORM32);
			SLLI(gpr.R(inst.dest), gpr.R(inst.src1), 16);
			SRAIW(gpr.R(inst.dest), gpr.R(inst.dest), 16);
		}
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Bits(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::ReverseBits:
		CompIR_Generic(inst);
		break;

	case IROp::BSwap16:
		CompIR_Generic(inst);
		break;

	case IROp::BSwap32:
		if (cpu_info.RiscV_Zbb) {
			gpr.MapDirtyIn(inst.dest, inst.src1);
			REV8(gpr.R(inst.dest), gpr.R(inst.src1));
			if (XLEN >= 64) {
				// REV8 swaps the entire register, so get the 32 highest bits.
				SRAI(gpr.R(inst.dest), gpr.R(inst.dest), XLEN - 32);
				gpr.MarkDirty(gpr.R(inst.dest), true);
			}
		} else {
			CompIR_Generic(inst);
		}
		break;

	case IROp::Clz:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Shift(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Shl:
	case IROp::Shr:
	case IROp::Sar:
	case IROp::Ror:
	case IROp::ShlImm:
	case IROp::ShrImm:
	case IROp::SarImm:
	case IROp::RorImm:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Compare(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Slt:
	case IROp::SltConst:
	case IROp::SltU:
	case IROp::SltUConst:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_CondAssign(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::MovZ:
	case IROp::MovNZ:
	case IROp::Max:
	case IROp::Min:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_HiLo(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::MtLo:
	case IROp::MtHi:
	case IROp::MfLo:
	case IROp::MfHi:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Mult(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Mult:
	case IROp::MultU:
	case IROp::Madd:
	case IROp::MaddU:
	case IROp::Msub:
	case IROp::MsubU:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

void RiscVJit::CompIR_Div(IRInst inst) {
	CONDITIONAL_DISABLE;

	switch (inst.op) {
	case IROp::Div:
	case IROp::DivU:
		CompIR_Generic(inst);
		break;

	default:
		INVALIDOP;
		break;
	}
}

} // namespace MIPSComp
