// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/bit_field.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

namespace Shader::Maxwell {
namespace {
IR::U32 Insert(TranslatorVisitor& v, const IR::U32& base, const IR::U32& insert, const IR::U32& field) {
    // a field past bit 31 is truncated; one starting past it changes no bits.
    if (field.IsImmediate()) {
        const u32 offset{field.U32() & 0xff};
        const u32 count{(field.U32() >> 8) & 0xff};
        if (count == 0 || offset >= 32) return base;
        const u32 size{std::min(count, 32 - offset)};
        return size == 32 ? insert : v.ir.BitFieldInsert(base, insert, v.ir.Imm32(offset), v.ir.Imm32(size));
    }
    const IR::U32 offset{v.ir.BitFieldExtract(field, v.ir.Imm32(0), v.ir.Imm32(8), false)};
    const IR::U32 count{v.ir.BitFieldExtract(field, v.ir.Imm32(8), v.ir.Imm32(8), false)};
    const IR::U32 max_size{v.ir.Imm32(32)};
    const IR::U32 remaining{v.ir.ISub(max_size, v.ir.UMin(offset, max_size))};
    const IR::U32 safe_count{v.ir.UMin(count, remaining)};
    const IR::U32 safe_offset{v.ir.UMin(offset, v.ir.Imm32(31))};
    return v.ir.BitFieldInsert(base, insert, safe_offset, safe_count);
}

void BFI(TranslatorVisitor& v, u64 insn, const IR::U32& src_a, const IR::U32& base) {
    union {
        u64 insn;
        BitField<0, 8, IR::Reg> dest_reg;
        BitField<8, 8, IR::Reg> insert_reg;
        BitField<47, 1, u64> cc;
    } const bfi{insn};

    const IR::U32 insert{v.X(bfi.insert_reg)};
    const IR::U32 result{Insert(v, base, insert, src_a)};

    v.X(bfi.dest_reg, result);
    if (bfi.cc != 0) {
        const IR::U32 zero{v.ir.Imm32(0)};
        v.SetZFlag(v.ir.IEqual(result, zero));
        v.SetSFlag(v.ir.ILessThan(result, zero, true));
        v.ResetCFlag();
        v.ResetOFlag();
    }
}
} // Anonymous namespace

void TranslatorVisitor::BFI_reg(u64 insn) {
    BFI(*this, insn, GetReg20(insn), GetReg39(insn));
}

void TranslatorVisitor::BFI_rc(u64 insn) {
    BFI(*this, insn, GetReg39(insn), GetCbuf(insn));
}

void TranslatorVisitor::BFI_cr(u64 insn) {
    BFI(*this, insn, GetCbuf(insn), GetReg39(insn));
}

void TranslatorVisitor::BFI_imm(u64 insn) {
    BFI(*this, insn, GetImm20(insn), GetReg39(insn));
}

} // namespace Shader::Maxwell
