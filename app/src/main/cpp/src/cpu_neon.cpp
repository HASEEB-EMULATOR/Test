#include "haseeb/cpu_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace haseeb::vm {

namespace {

uint64_t ClampToInt64(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 9223372036854775808.0) return static_cast<uint64_t>(INT64_MAX);
    if (v < -9223372036854775808.0) return static_cast<uint64_t>(INT64_MIN);
    return static_cast<uint64_t>(static_cast<int64_t>(v));
}

uint64_t ClampToUint64(double v) {
    if (std::isnan(v) || v < 0.0) return 0;
    if (v >= 18446744073709551616.0) return UINT64_MAX;
    return static_cast<uint64_t>(v);
}

int32_t ClampToInt32(double v) {
    if (std::isnan(v)) return 0;
    if (v >= 2147483648.0) return INT32_MAX;
    if (v < -2147483648.0) return INT32_MIN;
    return static_cast<int32_t>(v);
}

uint32_t ClampToUint32(double v) {
    if (std::isnan(v) || v < 0.0) return 0;
    if (v >= 4294967296.0) return UINT32_MAX;
    return static_cast<uint32_t>(v);
}

} // namespace

bool AArch64CpuCore::CheckFpSimdEnabled() {
    const uint32_t fpen = (state_.sys.cpacr_el1 >> 20) & 3u;
    if ((state_.pstate.el == 0 && fpen != 3u) || (state_.pstate.el == 1 && (fpen & 1u) == 0u)) {
        RaiseException(ec::kFpSimdAccess, 0x001E0000u, 0, false, state_.pc);
        return false;
    }
    return true;
}

bool AArch64CpuCore::ExecuteFpScalar(uint32_t insn) {
    if (!CheckFpSimdEnabled()) return false;

    const uint8_t rd = static_cast<uint8_t>(insn & 31u);
    const uint8_t rn = static_cast<uint8_t>((insn >> 5) & 31u);
    const uint8_t rm = static_cast<uint8_t>((insn >> 16) & 31u);
    const uint32_t ftype = (insn >> 22) & 3u; // 0=S(f32), 1=D(f64)

    // Floating-point <-> integer conversions & FMOV GPR<->FPR:
    // (insn & 0x5F200000) == 0x1E200000 && bit21 == 1 && bit15..10 == 000000
    if ((insn & 0x7F3FFC00u) == 0x1E200000u) {
        const uint8_t sf = static_cast<uint8_t>((insn >> 31) & 1u);
        const uint8_t rmode = static_cast<uint8_t>((insn >> 19) & 3u);
        const uint8_t opcode = static_cast<uint8_t>((insn >> 16) & 7u);

        if (rmode == 0u && opcode == 6u) {
            // FMOV Wd/Xd, Sn/Dn (FP -> GPR)
            uint64_t val = (ftype == 0u) ? state_.v[rn].u32[0] : state_.v[rn].u64[0];
            if (!sf) val &= 0xFFFFFFFFULL;
            state_.WriteRegOrDiscard(rd, val);
            return true;
        }
        if (rmode == 0u && opcode == 7u) {
            // FMOV Sd/Dd, Wn/Xn (GPR -> FP)
            uint64_t src = state_.ReadRegOrZero(rn);
            state_.v[rd].u64[0] = (ftype == 0u) ? (src & 0xFFFFFFFFULL) : src;
            state_.v[rd].u64[1] = 0;
            return true;
        }
        if (opcode == 2u || opcode == 3u) {
            // SCVTF / UCVTF (GPR -> FP)
            const uint64_t raw = state_.ReadRegOrZero(rn);
            const bool is_unsigned = (opcode == 3u);
            double dval = 0.0;
            if (sf) {
                dval = is_unsigned ? static_cast<double>(raw) : static_cast<double>(static_cast<int64_t>(raw));
            } else {
                uint32_t w = static_cast<uint32_t>(raw);
                dval = is_unsigned ? static_cast<double>(w) : static_cast<double>(static_cast<int32_t>(w));
            }
            if (ftype == 0u) {
                state_.v[rd].f32[0] = static_cast<float>(dval);
                state_.v[rd].u32[1] = 0;
                state_.v[rd].u64[1] = 0;
            } else {
                state_.v[rd].f64[0] = dval;
                state_.v[rd].u64[1] = 0;
            }
            return true;
        }
        if (rmode == 3u && (opcode == 0u || opcode == 1u)) {
            // FCVTZS (opcode=0) / FCVTZU (opcode=1) (FP -> GPR)
            double dval = (ftype == 0u) ? static_cast<double>(state_.v[rn].f32[0]) : state_.v[rn].f64[0];
            uint64_t out = 0;
            if (opcode == 0u) {
                out = sf ? ClampToInt64(dval) : static_cast<uint32_t>(ClampToInt32(dval));
            } else {
                out = sf ? ClampToUint64(dval) : ClampToUint32(dval);
            }
            state_.WriteRegOrDiscard(rd, out);
            return true;
        }
    }

    // FMOV (immediate): (insn & 0x7F3FE01F) -> bits[28:24]=11110, bit21=1, bit12..10=100
    if ((insn & 0x5FA01C00u) == 0x1E201000u && ((insn >> 21) & 1u) == 1u) {
        const uint8_t imm8 = static_cast<uint8_t>((insn >> 13) & 0xFFu);
        const uint64_t sign = (imm8 >> 7) & 1u;
        const uint64_t exp_top = ((imm8 >> 6) & 1u) ^ 1u;
        const uint64_t exp_rest = (imm8 >> 4) & 3u;
        const uint64_t mant = imm8 & 0xFu;
        if (ftype == 0u) {
            uint32_t bits = static_cast<uint32_t>((sign << 31) |
                            (((imm8 & 0x40u) ? 0x3Fu : 0x40u) << 23) |
                            (exp_top << 30) | (exp_rest << 23) | (mant << 19));
            // Exact VFPExpandImm for binary32: sign:not(b6):rep(b6,5):b5..b0:zeros(19)
            uint32_t rep = ((imm8 >> 6) & 1u) ? 0x1Fu : 0u;
            bits = static_cast<uint32_t>((sign << 31) | (exp_top << 30) | (rep << 25) |
                                         (exp_rest << 23) | (mant << 19));
            state_.v[rd].u32[0] = bits;
            state_.v[rd].u32[1] = 0;
            state_.v[rd].u64[1] = 0;
        } else {
            uint64_t rep = ((imm8 >> 6) & 1u) ? 0xFFULL : 0ULL;
            uint64_t bits = (sign << 63) | (exp_top << 62) | (rep << 54) |
                            (exp_rest << 52) | (mant << 48);
            state_.v[rd].u64[0] = bits;
            state_.v[rd].u64[1] = 0;
        }
        return true;
    }

    // Floating-point data-processing (1 source): bit21=1, bit14..10=10000
    if ((insn & 0x5FA07C00u) == 0x1E204000u && ((insn >> 21) & 1u) == 1u) {
        const uint8_t opc = static_cast<uint8_t>((insn >> 15) & 0x3Fu);
        if (ftype == 0u) {
            float a = state_.v[rn].f32[0];
            if (opc == 0u) state_.v[rd].f32[0] = a;                  // FMOV
            else if (opc == 1u) state_.v[rd].f32[0] = std::fabs(a);  // FABS
            else if (opc == 2u) state_.v[rd].f32[0] = -a;            // FNEG
            else if (opc == 3u) state_.v[rd].f32[0] = std::sqrt(a);  // FSQRT
            else if (opc == 5u) {                                    // FCVT Dd, Sn
                state_.v[rd].f64[0] = static_cast<double>(a);
                state_.v[rd].u64[1] = 0;
                return true;
            }
            state_.v[rd].u32[1] = 0;
            state_.v[rd].u64[1] = 0;
        } else {
            double a = state_.v[rn].f64[0];
            if (opc == 0u) state_.v[rd].f64[0] = a;
            else if (opc == 1u) state_.v[rd].f64[0] = std::fabs(a);
            else if (opc == 2u) state_.v[rd].f64[0] = -a;
            else if (opc == 3u) state_.v[rd].f64[0] = std::sqrt(a);
            else if (opc == 4u) {                                    // FCVT Sd, Dn
                state_.v[rd].f32[0] = static_cast<float>(a);
                state_.v[rd].u32[1] = 0;
                state_.v[rd].u64[1] = 0;
                return true;
            }
            state_.v[rd].u64[1] = 0;
        }
        return true;
    }

    // Floating-point compare (FCMP / FCMPE): bit21=1, bit13..10=1000
    if ((insn & 0x5FA03C00u) == 0x1E202000u && ((insn >> 21) & 1u) == 1u) {
        const bool cmp_zero = ((insn >> 3) & 1u) != 0;
        double a = (ftype == 0u) ? state_.v[rn].f32[0] : state_.v[rn].f64[0];
        double b = cmp_zero ? 0.0 : ((ftype == 0u) ? state_.v[rm].f32[0] : state_.v[rm].f64[0]);
        if (std::isnan(a) || std::isnan(b)) {
            state_.pstate.n = false;
            state_.pstate.z = false;
            state_.pstate.c = true;
            state_.pstate.v = true;
        } else if (a == b) {
            state_.pstate.n = false;
            state_.pstate.z = true;
            state_.pstate.c = true;
            state_.pstate.v = false;
        } else if (a < b) {
            state_.pstate.n = true;
            state_.pstate.z = false;
            state_.pstate.c = false;
            state_.pstate.v = false;
        } else {
            state_.pstate.n = false;
            state_.pstate.z = false;
            state_.pstate.c = true;
            state_.pstate.v = false;
        }
        return true;
    }

    // Floating-point conditional select (FCSEL): bit21=1, bit11..10=11
    if ((insn & 0x5FA00C00u) == 0x1E200C00u && ((insn >> 21) & 1u) == 1u) {
        const uint8_t cond = static_cast<uint8_t>((insn >> 12) & 0xFu);
        const uint8_t src = EvalCond(cond) ? rn : rm;
        if (ftype == 0u) {
            state_.v[rd].u32[0] = state_.v[src].u32[0];
            state_.v[rd].u32[1] = 0;
        } else {
            state_.v[rd].u64[0] = state_.v[src].u64[0];
        }
        state_.v[rd].u64[1] = 0;
        return true;
    }

    // Floating-point data-processing (2 source): bit21=1, bit11..10=10
    if ((insn & 0x5FA00C00u) == 0x1E200800u && ((insn >> 21) & 1u) == 1u) {
        const uint8_t opc = static_cast<uint8_t>((insn >> 12) & 0xFu);
        if (ftype == 0u) {
            float a = state_.v[rn].f32[0];
            float b = state_.v[rm].f32[0];
            float r = 0.0f;
            switch (opc) {
                case 0: r = a * b; break;
                case 1: r = a / b; break;
                case 2: r = a + b; break;
                case 3: r = a - b; break;
                case 4: r = std::fmax(a, b); break;
                case 5: r = std::fmin(a, b); break;
                default: break;
            }
            state_.v[rd].f32[0] = r;
            state_.v[rd].u32[1] = 0;
            state_.v[rd].u64[1] = 0;
        } else {
            double a = state_.v[rn].f64[0];
            double b = state_.v[rm].f64[0];
            double r = 0.0;
            switch (opc) {
                case 0: r = a * b; break;
                case 1: r = a / b; break;
                case 2: r = a + b; break;
                case 3: r = a - b; break;
                case 4: r = std::fmax(a, b); break;
                case 5: r = std::fmin(a, b); break;
                default: break;
            }
            state_.v[rd].f64[0] = r;
            state_.v[rd].u64[1] = 0;
        }
        return true;
    }

    // Floating-point data-processing (3 source): FMADD/FMSUB/FNMADD/FNMSUB (bits[28:24]=11111)
    if ((insn & 0x5F000000u) == 0x1F000000u) {
        const uint8_t ra = static_cast<uint8_t>((insn >> 10) & 31u);
        const bool o0 = ((insn >> 15) & 1u) != 0;
        const bool o1 = ((insn >> 21) & 1u) != 0;
        if (ftype == 0u) {
            float a = state_.v[rn].f32[0];
            float b = state_.v[rm].f32[0];
            float c = state_.v[ra].f32[0];
            if (o1) c = -c;
            float prod = (o0 ^ o1) ? -(a * b) : (a * b);
            state_.v[rd].f32[0] = c + prod;
            state_.v[rd].u32[1] = 0;
            state_.v[rd].u64[1] = 0;
        } else {
            double a = state_.v[rn].f64[0];
            double b = state_.v[rm].f64[0];
            double c = state_.v[ra].f64[0];
            if (o1) c = -c;
            double prod = (o0 ^ o1) ? -(a * b) : (a * b);
            state_.v[rd].f64[0] = c + prod;
            state_.v[rd].u64[1] = 0;
        }
        return true;
    }

    RaiseException(ec::kUnknown, 0, 0, false, state_.pc);
    return false;
}

bool AArch64CpuCore::ExecuteNeonSimd(uint32_t insn) {
    if (!CheckFpSimdEnabled()) return false;

    const uint8_t rd = static_cast<uint8_t>(insn & 31u);
    const uint8_t rn = static_cast<uint8_t>((insn >> 5) & 31u);
    const uint8_t rm = static_cast<uint8_t>((insn >> 16) & 31u);
    const bool q = ((insn >> 30) & 1u) != 0;
    const uint32_t size = (insn >> 22) & 3u;
    const uint32_t lanes_bytes = q ? 16u : 8u;

    // 1. AdvSIMD Modified Immediate (MOVI / ORR / BIC / MVNI): bits[28:24]=01111, bit23=0, bit19=0
    if ((insn & 0x9F000400u) == 0x0F000400u && ((insn >> 23) & 1u) == 0 && ((insn >> 19) & 1u) == 0) {
        const uint32_t cmode = (insn >> 12) & 0xFu;
        const uint32_t op = (insn >> 29) & 1u;
        const uint64_t abc = (insn >> 16) & 7u;
        const uint64_t defgh = (insn >> 5) & 0x1Fu;
        const uint64_t imm8 = (abc << 5) | defgh;
        uint64_t pattern64 = 0;

        if (cmode == 0xEu && op == 0u) {
            // MOVI Vd.8B / 16B, #imm8
            pattern64 = imm8 * 0x0101010101010101ULL;
        } else if (cmode == 0xEu && op == 1u) {
            // MOVI Vd.2D, #imm64 (each bit of imm8 expanded to 8 bits)
            for (int b = 0; b < 8; ++b) {
                if ((imm8 >> b) & 1u) {
                    pattern64 |= (0xFFULL << (b * 8));
                }
            }
        } else if ((cmode & 9u) == 0u) {
            // 32-bit shifted immediate
            uint32_t v32 = static_cast<uint32_t>(imm8) << (((cmode >> 1) & 3u) * 8u);
            if (op) v32 = ~v32;
            pattern64 = (static_cast<uint64_t>(v32) << 32) | v32;
        } else {
            pattern64 = imm8 * 0x0101010101010101ULL;
        }

        state_.v[rd].u64[0] = pattern64;
        state_.v[rd].u64[1] = q ? pattern64 : 0ULL;
        return true;
    }

    // 2. AdvSIMD Copy: DUP (general), DUP (element), UMOV, INS (general)
    // Encoding: 0 Q U 01110 imm5 0 imm4 1 Rn Rd
    if ((insn & 0x9F208400u) == 0x0E000400u) {
        const uint32_t u = (insn >> 29) & 1u;
        const uint32_t imm5 = (insn >> 16) & 0x1Fu;
        const uint32_t imm4 = (insn >> 11) & 0xFu;

        if (u == 0u && imm4 == 1u) {
            // DUP Vd.T, Wn/Xn (general)
            const uint64_t src = state_.ReadRegOrZero(rn);
            state_.v[rd] = NeonReg{};
            if (imm5 & 1u) {
                uint8_t b = static_cast<uint8_t>(src);
                for (uint32_t i = 0; i < lanes_bytes; ++i) state_.v[rd].u8[i] = b;
            } else if (imm5 & 2u) {
                uint16_t h = static_cast<uint16_t>(src);
                for (uint32_t i = 0; i < lanes_bytes / 2u; ++i) state_.v[rd].u16[i] = h;
            } else if (imm5 & 4u) {
                uint32_t w = static_cast<uint32_t>(src);
                for (uint32_t i = 0; i < lanes_bytes / 4u; ++i) state_.v[rd].u32[i] = w;
            } else if (imm5 & 8u) {
                for (uint32_t i = 0; i < lanes_bytes / 8u; ++i) state_.v[rd].u64[i] = src;
            }
            return true;
        }
        if (u == 0u && imm4 == 3u) {
            // INS Vd.Ts[index], Wn/Xn
            const uint64_t src = state_.ReadRegOrZero(rn);
            if (imm5 & 1u) state_.v[rd].u8[(imm5 >> 1) & 15u] = static_cast<uint8_t>(src);
            else if (imm5 & 2u) state_.v[rd].u16[(imm5 >> 2) & 7u] = static_cast<uint16_t>(src);
            else if (imm5 & 4u) state_.v[rd].u32[(imm5 >> 3) & 3u] = static_cast<uint32_t>(src);
            else if (imm5 & 8u) state_.v[rd].u64[(imm5 >> 4) & 1u] = src;
            return true;
        }
        if (u == 0u && imm4 == 7u) {
            // UMOV Wd/Xd, Vn.Ts[index]
            uint64_t val = 0;
            if (imm5 & 1u) val = state_.v[rn].u8[(imm5 >> 1) & 15u];
            else if (imm5 & 2u) val = state_.v[rn].u16[(imm5 >> 2) & 7u];
            else if (imm5 & 4u) val = state_.v[rn].u32[(imm5 >> 3) & 3u];
            else if (imm5 & 8u) val = state_.v[rn].u64[(imm5 >> 4) & 1u];
            state_.WriteRegOrDiscard(rd, val);
            return true;
        }
    }

    // 3. AdvSIMD Three Same: 0 Q U 01110 size 1 Rm opcode 1 Rn Rd
    if ((insn & 0x9F200400u) == 0x0E200400u) {
        const uint32_t u = (insn >> 29) & 1u;
        const uint32_t opcode = (insn >> 11) & 0x1Fu;
        NeonReg out{};
        const NeonReg& a = state_.v[rn];
        const NeonReg& b = state_.v[rm];

        // Bitwise logical ops (opcode == 0x03: AND/BIC/ORR/ORN/EOR/BSL/BIT/BIF encoded via size & u)
        if (opcode == 0x03u) {
            for (uint32_t i = 0; i < (q ? 2u : 1u); ++i) {
                uint64_t va = a.u64[i], vb = b.u64[i], vc = state_.v[rd].u64[i];
                if (u == 0u) {
                    if (size == 0u) out.u64[i] = va & vb;          // AND
                    else if (size == 1u) out.u64[i] = va & ~vb;    // BIC
                    else if (size == 2u) out.u64[i] = va | vb;     // ORR (MOV Vd, Vn when rn==rm)
                    else out.u64[i] = va | ~vb;                    // ORN
                } else {
                    if (size == 0u) out.u64[i] = va ^ vb;          // EOR
                    else if (size == 1u) out.u64[i] = (va & vc) | (vb & ~vc); // BSL
                    else if (size == 2u) out.u64[i] = (va & vb) | (vc & ~vb); // BIT
                    else out.u64[i] = (va & ~vb) | (vc & vb);                 // BIF
                }
            }
            state_.v[rd] = out;
            return true;
        }

        // Integer ADD (u=0, opcode=0x10) / SUB (u=1, opcode=0x10) / CMEQ (u=1, opcode=0x11)
        if (opcode == 0x10u || opcode == 0x11u) {
            const bool is_sub = (u == 1u && opcode == 0x10u);
            const bool is_cmeq = (u == 1u && opcode == 0x11u);
            if (size == 0u) {
                for (uint32_t i = 0; i < lanes_bytes; ++i) {
                    out.u8[i] = is_cmeq ? (a.u8[i] == b.u8[i] ? 0xFFu : 0u)
                                        : (is_sub ? (a.u8[i] - b.u8[i]) : (a.u8[i] + b.u8[i]));
                }
            } else if (size == 1u) {
                for (uint32_t i = 0; i < lanes_bytes / 2u; ++i) {
                    out.u16[i] = is_cmeq ? (a.u16[i] == b.u16[i] ? 0xFFFFu : 0u)
                                         : (is_sub ? (a.u16[i] - b.u16[i]) : (a.u16[i] + b.u16[i]));
                }
            } else if (size == 2u) {
                for (uint32_t i = 0; i < lanes_bytes / 4u; ++i) {
                    out.u32[i] = is_cmeq ? (a.u32[i] == b.u32[i] ? 0xFFFFFFFFu : 0u)
                                         : (is_sub ? (a.u32[i] - b.u32[i]) : (a.u32[i] + b.u32[i]));
                }
            } else {
                for (uint32_t i = 0; i < lanes_bytes / 8u; ++i) {
                    out.u64[i] = is_cmeq ? (a.u64[i] == b.u64[i] ? 0xFFFFFFFFFFFFFFFFULL : 0ULL)
                                         : (is_sub ? (a.u64[i] - b.u64[i]) : (a.u64[i] + b.u64[i]));
                }
            }
            state_.v[rd] = out;
            return true;
        }

        // Integer MUL (u=0, opcode=0x13)
        if (u == 0u && opcode == 0x13u) {
            if (size == 0u) {
                for (uint32_t i = 0; i < lanes_bytes; ++i) out.u8[i] = a.u8[i] * b.u8[i];
            } else if (size == 1u) {
                for (uint32_t i = 0; i < lanes_bytes / 2u; ++i) out.u16[i] = a.u16[i] * b.u16[i];
            } else if (size == 2u) {
                for (uint32_t i = 0; i < lanes_bytes / 4u; ++i) out.u32[i] = a.u32[i] * b.u32[i];
            }
            state_.v[rd] = out;
            return true;
        }

        // Floating-point vector FADD (u=0, size&1==0, opcode=0x1A) / FSUB (size&1==1, opcode=0x1A) / FMUL (u=1, size&1==0, opcode=0x1B)
        if (opcode == 0x1Au || opcode == 0x1Bu) {
            const bool is_f64 = (size & 2u) != 0;
            const bool is_sub = (opcode == 0x1Au) && ((size & 1u) != 0);
            const bool is_mul = (opcode == 0x1Bu) && (u == 1u);
            if (!is_f64) {
                for (uint32_t i = 0; i < lanes_bytes / 4u; ++i) {
                    out.f32[i] = is_mul ? (a.f32[i] * b.f32[i])
                                        : (is_sub ? (a.f32[i] - b.f32[i]) : (a.f32[i] + b.f32[i]));
                }
            } else if (q) {
                for (uint32_t i = 0; i < 2u; ++i) {
                    out.f64[i] = is_mul ? (a.f64[i] * b.f64[i])
                                        : (is_sub ? (a.f64[i] - b.f64[i]) : (a.f64[i] + b.f64[i]));
                }
            }
            state_.v[rd] = out;
            return true;
        }
    }

    // 4. AdvSIMD Across Lanes: UADDLV / ADDV / UMINV / UMAXV (0 Q U 01110 size 11000 opcode 10 Rn Rd)
    if ((insn & 0x9F3E0C00u) == 0x0E300800u) {
        const uint32_t u = (insn >> 29) & 1u;
        const uint32_t opcode = (insn >> 12) & 0x1Fu;
        const NeonReg& a = state_.v[rn];
        state_.v[rd] = NeonReg{};
        if (u == 1u && opcode == 0x03u) {
            // UADDLV
            uint64_t sum = 0;
            if (size == 0u) for (uint32_t i = 0; i < lanes_bytes; ++i) sum += a.u8[i];
            else if (size == 1u) for (uint32_t i = 0; i < lanes_bytes / 2u; ++i) sum += a.u16[i];
            else if (size == 2u) for (uint32_t i = 0; i < lanes_bytes / 4u; ++i) sum += a.u32[i];
            state_.v[rd].u64[0] = sum;
            return true;
        }
        if (u == 0u && opcode == 0x1Bu) {
            // ADDV
            uint64_t sum = 0;
            if (size == 0u) {
                for (uint32_t i = 0; i < lanes_bytes; ++i) sum += a.u8[i];
                state_.v[rd].u8[0] = static_cast<uint8_t>(sum);
            } else if (size == 1u) {
                for (uint32_t i = 0; i < lanes_bytes / 2u; ++i) sum += a.u16[i];
                state_.v[rd].u16[0] = static_cast<uint16_t>(sum);
            } else if (size == 2u) {
                for (uint32_t i = 0; i < lanes_bytes / 4u; ++i) sum += a.u32[i];
                state_.v[rd].u32[0] = static_cast<uint32_t>(sum);
            }
            return true;
        }
    }

    // 5. AdvSIMD EXT (byte extract across Vn:Vm)
    if ((insn & 0xBFE08400u) == 0x2E000000u) {
        const uint32_t imm4 = (insn >> 11) & 0xFu;
        uint8_t concat[32]{};
        std::memcpy(concat, state_.v[rn].u8, 16);
        std::memcpy(concat + 16, state_.v[rm].u8, 16);
        state_.v[rd] = NeonReg{};
        std::memcpy(state_.v[rd].u8, concat + imm4, lanes_bytes);
        return true;
    }

    // 6. AdvSIMD Two-reg misc: CNT / NOT (MVN)
    if ((insn & 0x9F3E0C00u) == 0x0E200800u) {
        const uint32_t u = (insn >> 29) & 1u;
        const uint32_t opcode = (insn >> 12) & 0x1Fu;
        if (u == 0u && opcode == 0x05u && size == 0u) {
            // CNT Vd.T, Vn.T
            NeonReg out{};
            for (uint32_t i = 0; i < lanes_bytes; ++i) {
                out.u8[i] = static_cast<uint8_t>(__builtin_popcount(state_.v[rn].u8[i]));
            }
            state_.v[rd] = out;
            return true;
        }
        if (u == 1u && opcode == 0x05u && size == 0u) {
            // NOT (MVN) Vd.T, Vn.T
            NeonReg out{};
            for (uint32_t i = 0; i < (q ? 2u : 1u); ++i) out.u64[i] = ~state_.v[rn].u64[i];
            state_.v[rd] = out;
            return true;
        }
    }

    RaiseException(ec::kUnknown, 0, 0, false, state_.pc);
    return false;
}

} // namespace haseeb::vm
