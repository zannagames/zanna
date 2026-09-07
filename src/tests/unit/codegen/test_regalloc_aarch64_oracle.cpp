//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/unit/codegen/test_regalloc_aarch64_oracle.cpp
// Purpose: Semantic oracle for the AArch64 function-wide allocator on hosts
//          that cannot run AArch64 code: a seeded generator builds random
//          edge-copy MIR functions (straight-line arithmetic, frame-slot
//          round trips, calls, diamonds, and nested counted loops carrying
//          many values through block parameters and ParallelCopy edges, in
//          both register classes), a small MIR interpreter executes the
//          function before allocation (virtual registers) and after it
//          (physical registers, frame memory, call clobbers as garbage), and
//          the two results must agree while the allocated function passes
//          the PostRA verifier.
// Key invariants:
//   - The interpreter models exactly what the allocator may rely on: a
//     call clobbers every caller-saved register, reads only x0, and returns
//     in x0; frame slots are plain memory; a parallel copy reads every
//     source before writing any destination.
//   - Set ZANNA_RA_ORACLE_SEEDS to widen the seed range.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/aarch64/ra/GlobalAllocator.hpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 3 C5)
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "codegen/aarch64/FrameBuilder.hpp"
#include "codegen/aarch64/MirVerify.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/passes/PassManager.hpp"
#include "codegen/aarch64/ra/GlobalAllocator.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace zanna::codegen::aarch64;

namespace {

// ---------------------------------------------------------------------------
// Interpreter
// ---------------------------------------------------------------------------

/// @brief Outcome of one interpretation.
struct RunResult {
    bool ok{false};
    uint64_t x0{0};
    std::string error;
};

/// @brief Executes the MIR subset the generator emits.
class Interpreter {
  public:
    explicit Interpreter(const MFunction &fn) : fn_(fn) {
        for (std::size_t i = 0; i < fn.blocks.size(); ++i)
            blockIndex_[fn.blocks[i].name] = i;
        phys_.fill(0);
        // Argument registers carry recognisable inputs.
        for (unsigned i = 0; i < 8; ++i)
            phys_[i] = 0x1000u + i;
    }

    RunResult run() {
        RunResult result;
        std::size_t bi = 0;
        std::size_t ii = 0;
        std::size_t steps = 0;
        while (true) {
            if (++steps > 2000000) {
                result.error = "step limit";
                return result;
            }
            if (bi >= fn_.blocks.size()) {
                result.error = "fell off the function";
                return result;
            }
            const auto &instrs = fn_.blocks[bi].instrs;
            if (ii >= instrs.size()) {
                // Fallthrough.
                ++bi;
                ii = 0;
                continue;
            }
            const MInstr &mi = instrs[ii];
            std::optional<std::size_t> jump;
            bool returned = false;
            if (!step(mi, jump, returned, result.error))
                return result;
            if (returned) {
                result.ok = true;
                result.x0 = phys_[0];
                return result;
            }
            if (jump) {
                bi = *jump;
                ii = 0;
            } else {
                ++ii;
            }
        }
    }

  private:
    const MFunction &fn_;
    std::unordered_map<std::string, std::size_t> blockIndex_;
    std::array<uint64_t, 64> phys_{};
    std::unordered_map<uint32_t, uint64_t> vregs_;
    std::map<int, uint64_t> mem_;
    int64_t cmpA_{0};
    int64_t cmpB_{0};

    static uint32_t vkey(const MReg &r) {
        return (static_cast<uint32_t>(r.cls) << 16) | r.idOrPhys;
    }

    bool read(const MOperand &op, uint64_t &out, std::string &err) {
        if (op.kind != MOperand::Kind::Reg) {
            err = "register operand expected";
            return false;
        }
        if (op.reg.isPhys) {
            out = phys_[op.reg.idOrPhys & 63u];
            return true;
        }
        auto it = vregs_.find(vkey(op.reg));
        if (it == vregs_.end()) {
            err = "use of undefined %v" + std::to_string(op.reg.idOrPhys);
            return false;
        }
        out = it->second;
        return true;
    }

    void write(const MOperand &op, uint64_t value) {
        if (op.reg.isPhys)
            phys_[op.reg.idOrPhys & 63u] = value;
        else
            vregs_[vkey(op.reg)] = value;
    }

    static double asDouble(uint64_t bits) {
        double d;
        std::memcpy(&d, &bits, sizeof d);
        return d;
    }

    static uint64_t asBits(double d) {
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof bits);
        return bits;
    }

    bool evalCond(const char *cond, std::string &err) const {
        const std::string c = cond ? cond : "";
        const auto ua = static_cast<uint64_t>(cmpA_);
        const auto ub = static_cast<uint64_t>(cmpB_);
        if (c == "eq")
            return cmpA_ == cmpB_;
        if (c == "ne")
            return cmpA_ != cmpB_;
        if (c == "lt")
            return cmpA_ < cmpB_;
        if (c == "le")
            return cmpA_ <= cmpB_;
        if (c == "gt")
            return cmpA_ > cmpB_;
        if (c == "ge")
            return cmpA_ >= cmpB_;
        if (c == "lo")
            return ua < ub;
        if (c == "ls")
            return ua <= ub;
        if (c == "hi")
            return ua > ub;
        if (c == "hs")
            return ua >= ub;
        err = "unsupported condition " + c;
        return false;
    }

    bool jumpTo(const std::string &label, std::optional<std::size_t> &jump, std::string &err) {
        auto it = blockIndex_.find(label);
        if (it == blockIndex_.end()) {
            err = "unknown block " + label;
            return false;
        }
        jump = it->second;
        return true;
    }

    /// The fake callee: reads x0, returns in x0, clobbers every caller-saved register.
    void call() {
        const uint64_t arg = phys_[0];
        for (unsigned i = 0; i <= 17; ++i)
            phys_[i] = 0xDEAD000000000000ull | i;
        for (unsigned i = 0; i < 8; ++i)
            phys_[32 + i] = 0xDEAD000000000000ull | (32 + i);
        for (unsigned i = 16; i < 32; ++i)
            phys_[32 + i] = 0xDEAD000000000000ull | (32 + i);
        phys_[0] = (arg * 0x9E3779B97F4A7C15ull) ^ (arg >> 7) ^ 0x5bd1e995ull;
    }

    bool step(const MInstr &mi,
              std::optional<std::size_t> &jump,
              bool &returned,
              std::string &err) {
        uint64_t a = 0;
        uint64_t b = 0;
        uint64_t c = 0;
        switch (mi.opc) {
            case MOpcode::MovRI:
                write(mi.ops[0], static_cast<uint64_t>(mi.ops[1].imm));
                return true;
            case MOpcode::MovRR:
            case MOpcode::FMovRR:
                if (!read(mi.ops[1], a, err))
                    return false;
                write(mi.ops[0], a);
                return true;
            case MOpcode::AddRRR:
                if (!read(mi.ops[1], a, err) || !read(mi.ops[2], b, err))
                    return false;
                write(mi.ops[0], a + b);
                return true;
            case MOpcode::SubRRR:
                if (!read(mi.ops[1], a, err) || !read(mi.ops[2], b, err))
                    return false;
                write(mi.ops[0], a - b);
                return true;
            case MOpcode::MulRRR:
                if (!read(mi.ops[1], a, err) || !read(mi.ops[2], b, err))
                    return false;
                write(mi.ops[0], a * b);
                return true;
            case MOpcode::EorRRR:
                if (!read(mi.ops[1], a, err) || !read(mi.ops[2], b, err))
                    return false;
                write(mi.ops[0], a ^ b);
                return true;
            case MOpcode::MAddRRRR:
                if (!read(mi.ops[1], a, err) || !read(mi.ops[2], b, err) ||
                    !read(mi.ops[3], c, err))
                    return false;
                write(mi.ops[0], c + a * b);
                return true;
            case MOpcode::AddRI:
                if (!read(mi.ops[1], a, err))
                    return false;
                write(mi.ops[0], a + static_cast<uint64_t>(mi.ops[2].imm));
                return true;
            case MOpcode::SubRI:
                if (!read(mi.ops[1], a, err))
                    return false;
                write(mi.ops[0], a - static_cast<uint64_t>(mi.ops[2].imm));
                return true;
            case MOpcode::LslRI:
                if (!read(mi.ops[1], a, err))
                    return false;
                write(mi.ops[0], a << (static_cast<unsigned>(mi.ops[2].imm) & 63u));
                return true;
            case MOpcode::FAddRRR:
                if (!read(mi.ops[1], a, err) || !read(mi.ops[2], b, err))
                    return false;
                write(mi.ops[0], asBits(asDouble(a) + asDouble(b)));
                return true;
            case MOpcode::CmpRI:
                if (!read(mi.ops[0], a, err))
                    return false;
                cmpA_ = static_cast<int64_t>(a);
                cmpB_ = mi.ops[1].imm;
                return true;
            case MOpcode::CmpRR:
                if (!read(mi.ops[0], a, err) || !read(mi.ops[1], b, err))
                    return false;
                cmpA_ = static_cast<int64_t>(a);
                cmpB_ = static_cast<int64_t>(b);
                return true;
            case MOpcode::Cset: {
                const bool taken = evalCond(mi.ops[1].cond, err);
                if (!err.empty())
                    return false;
                write(mi.ops[0], taken ? 1u : 0u);
                return true;
            }
            case MOpcode::BCond: {
                const bool taken = evalCond(mi.ops[0].cond, err);
                if (!err.empty())
                    return false;
                if (taken)
                    return jumpTo(mi.ops[1].label, jump, err);
                return true;
            }
            case MOpcode::Cbz:
            case MOpcode::Cbnz:
                if (!read(mi.ops[0], a, err))
                    return false;
                if ((a == 0) == (mi.opc == MOpcode::Cbz))
                    return jumpTo(mi.ops[1].label, jump, err);
                return true;
            case MOpcode::Br:
                return jumpTo(mi.ops[0].label, jump, err);
            case MOpcode::Ret:
                returned = true;
                return true;
            case MOpcode::LdrRegFpImm:
            case MOpcode::LdrFprFpImm: {
                const int off = static_cast<int>(mi.ops[1].imm);
                auto it = mem_.find(off);
                if (it == mem_.end()) {
                    err = "load from unwritten slot " + std::to_string(off);
                    return false;
                }
                write(mi.ops[0], it->second);
                return true;
            }
            case MOpcode::StrRegFpImm:
            case MOpcode::StrFprFpImm:
                if (!read(mi.ops[0], a, err))
                    return false;
                mem_[static_cast<int>(mi.ops[1].imm)] = a;
                return true;
            case MOpcode::Bl:
                call();
                return true;
            case MOpcode::ParallelCopy: {
                std::vector<uint64_t> values;
                for (std::size_t k = 1; k < mi.ops.size(); k += 2) {
                    if (!read(mi.ops[k], a, err))
                        return false;
                    values.push_back(a);
                }
                for (std::size_t k = 0; k + 1 < mi.ops.size(); k += 2)
                    write(mi.ops[k], values[k / 2]);
                return true;
            }
            default:
                err = std::string("unsupported opcode ") + opcodeName(mi.opc);
                return false;
        }
    }
};

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

/// @brief Small deterministic PRNG (xorshift64*).
class Rng {
  public:
    explicit Rng(uint64_t seed) : state_(seed * 2654435761ull + 0x9E3779B97F4A7C15ull) {
        if (state_ == 0)
            state_ = 1;
    }

    uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 2685821657736338717ull;
    }

    unsigned below(unsigned n) {
        return n == 0 ? 0 : static_cast<unsigned>(next() % n);
    }

    bool chance(unsigned percent) {
        return below(100) < percent;
    }

  private:
    uint64_t state_;
};

MOperand v(uint16_t id, RegClass cls = RegClass::GPR) {
    return MOperand::vregOp(cls, id);
}

MOperand x(PhysReg r) {
    return MOperand::regOp(r);
}

MOperand imm(long long value) {
    return MOperand::immOp(value);
}

MOperand label(const std::string &name) {
    return MOperand::labelOp(name);
}

MInstr ins(MOpcode opc, std::vector<MOperand> ops) {
    return MInstr{opc, std::move(ops)};
}

/// @brief Builds one random edge-copy MIR function.
class Generator {
  public:
    explicit Generator(uint64_t seed) : rng_(seed) {}

    MFunction build() {
        fn_ = MFunction{};
        fn_.name = "oracle";
        fn_.isLeaf = false;
        FrameBuilder fb(fn_);
        fb.addLocal(1, 8, 8);
        fb.addLocal(2, 8, 8);
        slotA_ = fb.localOffset(1);
        slotB_ = fb.localOffset(2);

        newBlock("entry");
        const unsigned k = 2 + rng_.below(30);
        for (unsigned i = 0; i < k; ++i)
            vals_.push_back(defineConst());
        // Some values come from the incoming argument registers.
        if (rng_.chance(50)) {
            const uint16_t id = fresh();
            emit(ins(MOpcode::MovRR, {v(id), x(PhysReg::X0)}));
            vals_.push_back(id);
        }
        const unsigned f = rng_.below(4);
        for (unsigned i = 0; i < f; ++i)
            fvals_.push_back(defineFloat());

        const unsigned regions = 1 + rng_.below(5);
        for (unsigned i = 0; i < regions; ++i)
            region(0);

        finish();
        return std::move(fn_);
    }

  private:
    Rng rng_;
    MFunction fn_;
    uint16_t nextId_{1};
    unsigned nextLabel_{0};
    std::vector<uint16_t> vals_;
    std::vector<uint16_t> fvals_;
    int slotA_{0};
    int slotB_{0};
    bool slotAWritten_{false};
    bool slotBWritten_{false};

    uint16_t fresh() {
        return nextId_++;
    }

    MBasicBlock &cur() {
        return fn_.blocks.back();
    }

    void emit(MInstr mi) {
        cur().instrs.push_back(std::move(mi));
    }

    std::string newBlock(const std::string &prefix) {
        MBasicBlock bb;
        bb.name = prefix + "_" + std::to_string(nextLabel_++);
        fn_.blocks.push_back(std::move(bb));
        return fn_.blocks.back().name;
    }

    uint16_t defineConst() {
        const uint16_t id = fresh();
        emit(ins(MOpcode::MovRI, {v(id), imm(static_cast<long long>(rng_.below(4000)) - 1000)}));
        return id;
    }

    uint16_t defineFloat() {
        // Reinterpret a GPR value through a slot: both worlds agree bit for bit.
        const uint16_t src = vals_[rng_.below(static_cast<unsigned>(vals_.size()))];
        emit(ins(MOpcode::StrRegFpImm, {v(src), imm(slotB_)}));
        slotBWritten_ = true;
        const uint16_t id = fresh();
        emit(ins(MOpcode::LdrFprFpImm, {v(id, RegClass::FPR), imm(slotB_)}));
        return id;
    }

    uint16_t pick() {
        return vals_[rng_.below(static_cast<unsigned>(vals_.size()))];
    }

    /// Replace a random live value (or append while below the cap).
    void publish(uint16_t id) {
        if (vals_.size() < 40 && rng_.chance(35))
            vals_.push_back(id);
        else
            vals_[rng_.below(static_cast<unsigned>(vals_.size()))] = id;
    }

    void op() {
        switch (rng_.below(12)) {
            case 0:
                publish(defineConst());
                break;
            case 1: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::AddRRR, {v(id), v(pick()), v(pick())}));
                publish(id);
                break;
            }
            case 2: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::SubRRR, {v(id), v(pick()), v(pick())}));
                publish(id);
                break;
            }
            case 3: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::MulRRR, {v(id), v(pick()), v(pick())}));
                publish(id);
                break;
            }
            case 4: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::EorRRR, {v(id), v(pick()), v(pick())}));
                publish(id);
                break;
            }
            case 5: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::AddRI, {v(id), v(pick()), imm(rng_.below(4000))}));
                publish(id);
                break;
            }
            case 6: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::LslRI, {v(id), v(pick()), imm(rng_.below(8))}));
                publish(id);
                break;
            }
            case 7: {
                // Round trip through the first slot.
                emit(ins(MOpcode::StrRegFpImm, {v(pick()), imm(slotA_)}));
                slotAWritten_ = true;
                const uint16_t id = fresh();
                emit(ins(MOpcode::LdrRegFpImm, {v(id), imm(slotA_)}));
                publish(id);
                break;
            }
            case 8: {
                // A call: marshal, call, capture.
                emit(ins(MOpcode::MovRR, {x(PhysReg::X0), v(pick())}));
                emit(ins(MOpcode::Bl, {label("callee")}));
                const uint16_t id = fresh();
                emit(ins(MOpcode::MovRR, {v(id), x(PhysReg::X0)}));
                publish(id);
                break;
            }
            case 9: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::MAddRRRR, {v(id), v(pick()), v(pick()), v(pick())}));
                publish(id);
                break;
            }
            case 10: {
                const uint16_t id = fresh();
                emit(ins(MOpcode::CmpRR, {v(pick()), v(pick())}));
                emit(ins(MOpcode::Cset, {v(id), MOperand::condOp("lt")}));
                publish(id);
                break;
            }
            default: {
                if (fvals_.empty()) {
                    publish(defineConst());
                    break;
                }
                const uint16_t id = fresh();
                const uint16_t a = fvals_[rng_.below(static_cast<unsigned>(fvals_.size()))];
                const uint16_t b = fvals_[rng_.below(static_cast<unsigned>(fvals_.size()))];
                emit(ins(MOpcode::FAddRRR,
                         {v(id, RegClass::FPR), v(a, RegClass::FPR), v(b, RegClass::FPR)}));
                fvals_[rng_.below(static_cast<unsigned>(fvals_.size()))] = id;
                break;
            }
        }
    }

    void straight() {
        const unsigned n = 1 + rng_.below(8);
        for (unsigned i = 0; i < n; ++i)
            op();
    }

    /// Emit a parallel copy from the current value sets into fresh params
    /// and return the params (GPR then FPR).
    std::pair<std::vector<uint16_t>, std::vector<uint16_t>> copyToParams(
        const std::vector<uint16_t> &gprParams, const std::vector<uint16_t> &fprParams) {
        std::vector<MOperand> ops;
        for (std::size_t i = 0; i < gprParams.size(); ++i) {
            ops.push_back(v(gprParams[i]));
            ops.push_back(v(vals_[i]));
        }
        for (std::size_t i = 0; i < fprParams.size(); ++i) {
            ops.push_back(v(fprParams[i], RegClass::FPR));
            ops.push_back(v(fvals_[i], RegClass::FPR));
        }
        if (!ops.empty())
            emit(ins(MOpcode::ParallelCopy, std::move(ops)));
        return {gprParams, fprParams};
    }

    std::vector<uint16_t> freshParams(std::size_t n) {
        std::vector<uint16_t> out;
        for (std::size_t i = 0; i < n; ++i)
            out.push_back(fresh());
        return out;
    }

    void diamond(unsigned depth) {
        // Shuffle the value order so a join permutes registers.
        const std::vector<uint16_t> savedVals = vals_;
        const std::vector<uint16_t> savedF = fvals_;
        const std::string right = "right_" + std::to_string(nextLabel_);
        const std::string join = "join_" + std::to_string(nextLabel_);
        const std::vector<uint16_t> gp = freshParams(vals_.size());
        const std::vector<uint16_t> fp = freshParams(fvals_.size());

        emit(ins(MOpcode::CmpRI, {v(pick()), imm(static_cast<long long>(rng_.below(200)) - 100)}));
        emit(ins(MOpcode::BCond, {MOperand::condOp(rng_.chance(50) ? "lt" : "ge"), label(right)}));

        // Left arm (fallthrough).
        newBlock("left");
        straight();
        if (rng_.chance(50) && depth < 2)
            region(depth + 1);
        // Keep the param count: trim or extend the live set to the param size.
        fitTo(savedVals.size(), savedF.size());
        rotate();
        copyToParams(gp, fp);
        emit(ins(MOpcode::Br, {label(join)}));

        // Right arm.
        vals_ = savedVals;
        fvals_ = savedF;
        fn_.blocks.push_back(MBasicBlock{});
        cur().name = right;
        ++nextLabel_;
        straight();
        fitTo(savedVals.size(), savedF.size());
        copyToParams(gp, fp);
        emit(ins(MOpcode::Br, {label(join)}));

        fn_.blocks.push_back(MBasicBlock{});
        cur().name = join;
        ++nextLabel_;
        vals_ = gp;
        fvals_ = fp;
    }

    /// Trim or extend the live sets to exactly the given sizes.
    void fitTo(std::size_t g, std::size_t f) {
        while (vals_.size() > g)
            vals_.pop_back();
        while (vals_.size() < g)
            vals_.push_back(defineConst());
        while (fvals_.size() > f)
            fvals_.pop_back();
        while (fvals_.size() < f)
            fvals_.push_back(defineFloat());
    }

    /// Rotate the value order (a permutation of registers at the join).
    void rotate() {
        if (vals_.size() > 1 && rng_.chance(60))
            std::rotate(vals_.begin(), vals_.begin() + 1, vals_.end());
    }

    void loop(unsigned depth) {
        const std::string header = "header_" + std::to_string(nextLabel_);
        const std::string body = "body_" + std::to_string(nextLabel_);
        const std::string exit = "exit_" + std::to_string(nextLabel_);
        const std::vector<uint16_t> gp = freshParams(vals_.size());
        const std::vector<uint16_t> fp = freshParams(fvals_.size());
        const uint16_t ctrParam = fresh();
        const uint16_t ctr0 = fresh();
        const long long trip = static_cast<long long>(rng_.below(5));

        emit(ins(MOpcode::MovRI, {v(ctr0), imm(0)}));
        {
            std::vector<MOperand> ops;
            for (std::size_t i = 0; i < gp.size(); ++i) {
                ops.push_back(v(gp[i]));
                ops.push_back(v(vals_[i]));
            }
            for (std::size_t i = 0; i < fp.size(); ++i) {
                ops.push_back(v(fp[i], RegClass::FPR));
                ops.push_back(v(fvals_[i], RegClass::FPR));
            }
            ops.push_back(v(ctrParam));
            ops.push_back(v(ctr0));
            emit(ins(MOpcode::ParallelCopy, std::move(ops)));
        }
        emit(ins(MOpcode::Br, {label(header)}));

        fn_.blocks.push_back(MBasicBlock{});
        cur().name = header;
        ++nextLabel_;
        emit(ins(MOpcode::CmpRI, {v(ctrParam), imm(trip)}));
        emit(ins(MOpcode::BCond, {MOperand::condOp("ge"), label(exit)}));

        fn_.blocks.push_back(MBasicBlock{});
        cur().name = body;
        ++nextLabel_;
        vals_ = gp;
        fvals_ = fp;
        straight();
        if (rng_.chance(40) && depth < 2)
            region(depth + 1);
        fitTo(gp.size(), fp.size());
        rotate();
        const uint16_t ctr1 = fresh();
        emit(ins(MOpcode::AddRI, {v(ctr1), v(ctrParam), imm(1)}));
        {
            std::vector<MOperand> ops;
            for (std::size_t i = 0; i < gp.size(); ++i) {
                ops.push_back(v(gp[i]));
                ops.push_back(v(vals_[i]));
            }
            for (std::size_t i = 0; i < fp.size(); ++i) {
                ops.push_back(v(fp[i], RegClass::FPR));
                ops.push_back(v(fvals_[i], RegClass::FPR));
            }
            ops.push_back(v(ctrParam));
            ops.push_back(v(ctr1));
            emit(ins(MOpcode::ParallelCopy, std::move(ops)));
        }
        emit(ins(MOpcode::Br, {label(header)}));

        fn_.blocks.push_back(MBasicBlock{});
        cur().name = exit;
        ++nextLabel_;
        vals_ = gp;
        fvals_ = fp;
        // The counter is observable too.
        vals_.push_back(ctrParam);
    }

    void region(unsigned depth) {
        switch (rng_.below(3)) {
            case 0:
                straight();
                break;
            case 1:
                diamond(depth);
                break;
            default:
                loop(depth);
                break;
        }
    }

    void finish() {
        // Fold every live value into x0 so each one is observed.
        uint16_t acc = vals_[0];
        for (std::size_t i = 1; i < vals_.size(); ++i) {
            const uint16_t id = fresh();
            emit(ins(MOpcode::EorRRR, {v(id), v(acc), v(vals_[i])}));
            acc = id;
        }
        for (uint16_t f : fvals_) {
            emit(ins(MOpcode::StrFprFpImm, {v(f, RegClass::FPR), imm(slotB_)}));
            const uint16_t bits = fresh();
            emit(ins(MOpcode::LdrRegFpImm, {v(bits), imm(slotB_)}));
            const uint16_t id = fresh();
            emit(ins(MOpcode::EorRRR, {v(id), v(acc), v(bits)}));
            acc = id;
        }
        emit(ins(MOpcode::MovRR, {x(PhysReg::X0), v(acc)}));
        emit(ins(MOpcode::Ret, {}));
    }
};

/// @brief Run one seed: generate, interpret, allocate, verify, interpret, compare.
bool checkSeed(uint64_t seed, std::string &why) {
    Generator gen(seed);
    MFunction fn = gen.build();

    {
        passes::Diagnostics diags;
        if (!verifyMir(fn, VerifyStage::PostLowering, darwinTarget(), diags)) {
            std::ostringstream os;
            diags.flush(os, &os);
            why = "generated MIR does not verify: " + os.str();
            return false;
        }
    }

    Interpreter before(fn);
    const RunResult expected = before.run();
    if (!expected.ok) {
        why = "pre-RA interpretation failed: " + expected.error;
        return false;
    }

    MFunction allocated = fn;
    try {
        (void)ra::allocateGlobal(allocated, darwinTarget());
    } catch (const std::exception &ex) {
        why = std::string("allocation threw: ") + ex.what();
        return false;
    }
    {
        passes::Diagnostics diags;
        if (!verifyMir(allocated, VerifyStage::PostRA, darwinTarget(), diags)) {
            std::ostringstream os;
            diags.flush(os, &os);
            why = "allocated MIR does not verify: " + os.str();
            return false;
        }
    }

    Interpreter after(allocated);
    const RunResult actual = after.run();
    if (!actual.ok) {
        why = "post-RA interpretation failed: " + actual.error;
        return false;
    }
    if (actual.x0 != expected.x0) {
        std::ostringstream os;
        os << "result mismatch: expected " << expected.x0 << " got " << actual.x0;
        why = os.str();
        return false;
    }
    return true;
}

unsigned seedCount() {
    if (const char *value = std::getenv("ZANNA_RA_ORACLE_SEEDS")) {
        const long n = std::strtol(value, nullptr, 10);
        if (n > 0)
            return static_cast<unsigned>(n);
    }
    return 1000;
}

} // namespace

TEST(Arm64GlobalRegAllocOracle, InterpreterAgreesWithItselfOnAStraightLine) {
    MFunction fn;
    fn.name = "s";
    MBasicBlock bb;
    bb.name = "entry";
    bb.instrs.push_back(ins(MOpcode::MovRI, {v(1), imm(6)}));
    bb.instrs.push_back(ins(MOpcode::MovRI, {v(2), imm(7)}));
    bb.instrs.push_back(ins(MOpcode::MulRRR, {v(3), v(1), v(2)}));
    bb.instrs.push_back(ins(MOpcode::MovRR, {x(PhysReg::X0), v(3)}));
    bb.instrs.push_back(ins(MOpcode::Ret, {}));
    fn.blocks.push_back(std::move(bb));
    Interpreter interp(fn);
    const RunResult r = interp.run();
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.x0, 42u);
}

TEST(Arm64GlobalRegAllocOracle, RandomFunctionsKeepTheirMeaning) {
    const unsigned seeds = seedCount();
    unsigned failures = 0;
    for (unsigned seed = 1; seed <= seeds; ++seed) {
        std::string why;
        if (!checkSeed(seed, why)) {
            ++failures;
            std::cerr << "seed " << seed << ": " << why << "\n";
            if (failures > 5)
                break;
        }
    }
    EXPECT_EQ(failures, 0u);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, &argv);
    return zanna_test::run_all_tests();
}
