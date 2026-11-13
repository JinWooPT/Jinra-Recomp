// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
IR::U32 Extract(TranslatorVisitor& v, const IR::U32& base, const IR::U32& field, bool is_signed) {
    const IR::U32 zero{v.ir.Imm32(0)};
    const IR::U32 last_bit{v.ir.Imm32(31)};
    // truncate a field at the word boundary before emitting SPIR-V.
    if (field.IsImmediate()) {
        const u32 offset{field.U32() & 0xff};
        const u32 count{(field.U32() >> 8) & 0xff};
        if (count == 0) return zero;
        if (offset >= 32) return is_signed ? IR::U32{v.ir.ShiftRightArithmetic(base, last_bit)} : zero;
        const u32 size{std::min(count, 32 - offset)};
        return size == 32 ? base : v.ir.BitFieldExtract(base, v.ir.Imm32(offset), v.ir.Imm32(size), is_signed);
    }
    const IR::U32 offset{v.ir.BitFieldExtract(field, zero, v.ir.Imm32(8), false)};
    const IR::U32 count{v.ir.BitFieldExtract(field, v.ir.Imm32(8), v.ir.Imm32(8), false)};
    const IR::U32 max_size{v.ir.Imm32(32)};
    const IR::U32 remaining{v.ir.ISub(max_size, v.ir.UMin(offset, max_size))};
    const IR::U32 safe_count{v.ir.UMin(count, remaining)};
    const IR::U32 safe_offset{v.ir.UMin(offset, last_bit)};
    const IR::U32 result{v.ir.BitFieldExtract(base, safe_offset, safe_count, is_signed)};
    if (!is_signed) return result;
    const IR::U1 replicate{v.ir.LogicalAnd(v.ir.IGreaterThanEqual(offset, max_size, false), v.ir.INotEqual(count, zero))};
    return IR::U32{v.ir.Select(replicate, v.ir.ShiftRightArithmetic(base, last_bit), result)};
}

void BFE(TranslatorVisitor& v, u64 insn, const IR::U32& src) {
    union {
        u64 insn;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> offset_reg;
        BitField<40, 1, u64> brev;
        BitField<47, 1, u64> cc;
        BitField<48, 1, u64> is_signed;
    } const bfe{insn};

    IR::U32 base{v.X(bfe.offset_reg)};
    if (bfe.brev != 0) {
        base = v.ir.BitReverse(base);
    }
    const IR::U32 result{Extract(v, base, src, bfe.is_signed != 0)};

    v.X(bfe.dest_reg, result);

    if (bfe.cc != 0) {
        const IR::U32 zero{v.ir.Imm32(0)};
        v.SetZFlag(v.ir.IEqual(result, zero));
        v.SetSFlag(v.ir.ILessThan(result, zero, true));
        v.ResetCFlag();
        v.ResetOFlag();
    }
}
} // Anonymous namespace

void TranslatorVisitor::BFE_reg(u64 insn) {
    BFE(*this, insn, GetReg20(insn));
}

void TranslatorVisitor::BFE_cbuf(u64 insn) {
    BFE(*this, insn, GetCbuf(insn));
}

void TranslatorVisitor::BFE_imm(u64 insn) {
    BFE(*this, insn, GetImm20(insn));
}

} // namespace Shader::Maxwell
