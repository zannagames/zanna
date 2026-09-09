//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/common/ILKernelGenerator.cpp
// Purpose: Implements the seeded IL kernel program generator.
// Key invariants:
//   - Each stage receives `(sum, i)` through block parameters, keeps every
//     intermediate below 2^47 (sum is re-masked to 28 bits between stages,
//     i stays below 512, constants stay small), and hands `(sum', i)` to the
//     next stage, so no checked operation can trap.
//   - Stage-local temporaries carry a per-stage suffix; labels are unique
//     per program.
//   - Only IL core opcodes are emitted; the text parses and verifies.
// Ownership/Lifetime:
//   - Stateless; a local RNG seeded from the argument drives every choice.
// Links: src/tests/common/ILKernelGenerator.hpp
//
//===----------------------------------------------------------------------===//

#include "common/ILKernelGenerator.hpp"

#include <random>
#include <sstream>

namespace zanna::tests {

namespace {

/// @brief 28-bit accumulator mask applied between stages.
constexpr long long kSumMask = 268435455;

/// @brief Emits one stage of the loop body.
class StageWriter {
  public:
    StageWriter(std::ostringstream &out, std::mt19937_64 &rng, std::size_t stage)
        : out_(out), rng_(rng), stage_(stage) {}

    /// @brief Random integer in `[lo, hi]`.
    long long pick(long long lo, long long hi) {
        std::uniform_int_distribution<long long> dist(lo, hi);
        return dist(rng_);
    }

    /// @brief Stage-local temporary name.
    std::string t(const char *base) const {
        return "%" + std::string(base) + "_" + std::to_string(stage_);
    }

    /// @brief Stage-local label.
    std::string l(const char *base) const {
        return std::string(base) + "_" + std::to_string(stage_);
    }

    /// @brief Label of the next stage (or the loop tail).
    std::string next(std::size_t stageCount) const {
        return stage_ + 1 < stageCount ? "st_" + std::to_string(stage_ + 1) : "tail";
    }

    /// @brief `br next(sum, i)` closing the stage.
    void finish(const std::string &sum, const std::string &i, std::size_t stageCount) {
        out_ << "  br " << next(stageCount) << "(" << sum << ", " << i << ")\n\n";
    }

    void checkedArith(const std::string &s, const std::string &i, std::size_t n) {
        // x in [0, 65535]; three multiplies by <= 16 keep the chain below 2^32.
        out_ << "  " << t("x") << " = and " << i << ", 65535\n";
        std::string cur = t("x");
        const int steps = static_cast<int>(pick(3, 6));
        int mulCount = 0;
        for (int k = 0; k < steps; ++k) {
            const std::string name = t(("a" + std::to_string(k)).c_str());
            const long long r = pick(0, 2);
            if (r == 0 && mulCount < 3) {
                out_ << "  " << name << " = imul.ovf " << cur << ", " << pick(2, 16) << "\n";
                ++mulCount;
            } else if (r == 1) {
                out_ << "  " << name << " = isub.ovf " << cur << ", " << pick(0, 40) << "\n";
            } else {
                out_ << "  " << name << " = iadd.ovf " << cur << ", " << pick(1, 40) << "\n";
            }
            cur = name;
        }
        out_ << "  " << t("acc") << " = iadd.ovf " << s << ", " << cur << "\n";
        out_ << "  " << t("m") << " = and " << t("acc") << ", " << kSumMask << "\n";
        finish(t("m"), i, n);
    }

    void idxChkReuse(const std::string &s, const std::string &i, std::size_t n) {
        // j in [lo, lo + 64): the check never trips; the normalized result is
        // consumed on both sides of a diamond (the A1 shape).
        const long long lo = pick(0, 100);
        out_ << "  " << t("j0") << " = and " << i << ", 63\n";
        out_ << "  " << t("j") << " = iadd.ovf " << t("j0") << ", " << lo << "\n";
        out_ << "  " << t("idx") << ":i64 = idx.chk " << t("j") << ", " << lo << ", " << lo + 64
             << "\n";
        out_ << "  " << t("bit") << " = and " << i << ", " << (1LL << pick(0, 5)) << "\n";
        out_ << "  " << t("c") << " = icmp_ne " << t("bit") << ", 0\n";
        out_ << "  cbr " << t("c") << ", " << l("ia") << "(" << s << ", " << t("idx") << "), "
             << l("ib") << "(" << s << ", " << t("idx") << ")\n\n";
        out_ << l("ia") << "(" << t("sa") << ": i64, " << t("xa") << ": i64):\n";
        out_ << "  " << t("va") << " = imul.ovf " << t("xa") << ", " << pick(2, 9) << "\n";
        out_ << "  " << t("ra") << " = iadd.ovf " << t("sa") << ", " << t("va") << "\n";
        out_ << "  br " << l("ij") << "(" << t("ra") << ")\n\n";
        out_ << l("ib") << "(" << t("sb") << ": i64, " << t("xb") << ": i64):\n";
        out_ << "  " << t("vb") << " = shl " << t("xb") << ", " << pick(1, 4) << "\n";
        out_ << "  " << t("rb") << " = iadd.ovf " << t("sb") << ", " << t("vb") << "\n";
        out_ << "  br " << l("ij") << "(" << t("rb") << ")\n\n";
        out_ << l("ij") << "(" << t("sj") << ": i64):\n";
        out_ << "  " << t("m") << " = and " << t("sj") << ", " << kSumMask << "\n";
        finish(t("m"), i, n);
    }

    void divRemConst(const std::string &s, const std::string &i, std::size_t n) {
        // Non-negative dividends (masked) and non-zero constant divisors: no
        // trap, no INT64_MIN / -1.
        static const char *const kOps[] = {"sdiv.chk0", "srem.chk0", "udiv.chk0", "urem.chk0"};
        out_ << "  " << t("d0") << " = and " << s << ", 1048575\n";
        out_ << "  " << t("d1") << " = imul.ovf " << i << ", " << pick(3, 31) << "\n";
        out_ << "  " << t("d2") << " = iadd.ovf " << t("d0") << ", " << t("d1") << "\n";
        std::string cur = t("d2");
        const int steps = static_cast<int>(pick(2, 4));
        std::string acc = s;
        for (int k = 0; k < steps; ++k) {
            const std::string name = t(("q" + std::to_string(k)).c_str());
            const char *op = kOps[pick(0, 3)];
            out_ << "  " << name << " = " << op << " " << cur << ", " << pick(1, 97) << "\n";
            const std::string sum = t(("qs" + std::to_string(k)).c_str());
            out_ << "  " << sum << " = iadd.ovf " << acc << ", " << name << "\n";
            acc = sum;
            // Feed the quotient/remainder (bounded by the dividend) forward.
            cur = name;
        }
        out_ << "  " << t("m") << " = and " << acc << ", " << kSumMask << "\n";
        finish(t("m"), i, n);
    }

    void switchDispatch(const std::string &s, const std::string &i, std::size_t n) {
        const long long cases = pick(2, 6);
        out_ << "  " << t("k64") << " = and " << i << ", 7\n";
        out_ << "  " << t("k") << ":i32 = cast.si_narrow.chk " << t("k64") << "\n";
        out_ << "  switch.i32 " << t("k") << ", ^" << l("sd") << "(" << s << ", " << i << ")";
        for (long long c = 0; c < cases; ++c) {
            out_ << ", " << c << " -> ^" << l(("sc" + std::to_string(c)).c_str()) << "(" << s
                 << ", " << i << ")";
        }
        out_ << "\n\n";
        for (long long c = 0; c < cases; ++c) {
            const std::string lab = l(("sc" + std::to_string(c)).c_str());
            const std::string sv = t(("scs" + std::to_string(c)).c_str());
            const std::string iv = t(("sci" + std::to_string(c)).c_str());
            const std::string v = t(("scv" + std::to_string(c)).c_str());
            out_ << lab << "(" << sv << ": i64, " << iv << ": i64):\n";
            if (c % 2 == 0)
                out_ << "  " << v << " = iadd.ovf " << sv << ", " << pick(1, 60) << "\n";
            else
                out_ << "  " << v << " = xor " << sv << ", " << pick(1, 4095) << "\n";
            out_ << "  br " << l("sj") << "(" << v << ", " << iv << ")\n\n";
        }
        out_ << l("sd") << "(" << t("sds") << ": i64, " << t("sdi") << ": i64):\n";
        out_ << "  " << t("sdv") << " = iadd.ovf " << t("sds") << ", 1\n";
        out_ << "  br " << l("sj") << "(" << t("sdv") << ", " << t("sdi") << ")\n\n";
        out_ << l("sj") << "(" << t("sjs") << ": i64, " << t("sji") << ": i64):\n";
        out_ << "  " << t("m") << " = and " << t("sjs") << ", " << kSumMask << "\n";
        finish(t("m"), t("sji"), n);
    }

    void selectDiamond(const std::string &s, const std::string &i, std::size_t n) {
        // min(x, y) then clamp to a bound, both through diamonds.
        const long long bound = pick(256, 8191);
        out_ << "  " << t("h0") << " = imul.ovf " << i << ", 2654435761\n";
        out_ << "  " << t("h1") << " = lshr " << t("h0") << ", " << pick(5, 17) << "\n";
        out_ << "  " << t("h2") << " = xor " << t("h0") << ", " << t("h1") << "\n";
        out_ << "  " << t("x") << " = and " << t("h2") << ", 8191\n";
        out_ << "  " << t("y") << " = and " << s << ", 8191\n";
        out_ << "  " << t("lt") << " = scmp_lt " << t("x") << ", " << t("y") << "\n";
        out_ << "  cbr " << t("lt") << ", " << l("mx") << "(" << s << ", " << i << ", " << t("x")
             << "), " << l("my") << "(" << s << ", " << i << ", " << t("y") << ")\n\n";
        out_ << l("mx") << "(" << t("s0") << ": i64, " << t("i0") << ": i64, " << t("m0")
             << ": i64):\n";
        out_ << "  br " << l("cl") << "(" << t("s0") << ", " << t("i0") << ", " << t("m0")
             << ")\n\n";
        out_ << l("my") << "(" << t("s1") << ": i64, " << t("i1") << ": i64, " << t("m1")
             << ": i64):\n";
        out_ << "  br " << l("cl") << "(" << t("s1") << ", " << t("i1") << ", " << t("m1")
             << ")\n\n";
        out_ << l("cl") << "(" << t("s2") << ": i64, " << t("i2") << ": i64, " << t("m2")
             << ": i64):\n";
        out_ << "  " << t("big") << " = scmp_gt " << t("m2") << ", " << bound << "\n";
        out_ << "  cbr " << t("big") << ", " << l("ub") << "(" << t("s2") << ", " << t("i2")
             << "), " << l("um") << "(" << t("s2") << ", " << t("i2") << ", " << t("m2") << ")\n\n";
        out_ << l("ub") << "(" << t("s3") << ": i64, " << t("i3") << ": i64):\n";
        out_ << "  br " << l("dj") << "(" << t("s3") << ", " << t("i3") << ", " << bound << ")\n\n";
        out_ << l("um") << "(" << t("s4") << ": i64, " << t("i4") << ": i64, " << t("m4")
             << ": i64):\n";
        out_ << "  br " << l("dj") << "(" << t("s4") << ", " << t("i4") << ", " << t("m4")
             << ")\n\n";
        out_ << l("dj") << "(" << t("s5") << ": i64, " << t("i5") << ": i64, " << t("v")
             << ": i64):\n";
        out_ << "  " << t("acc") << " = iadd.ovf " << t("s5") << ", " << t("v") << "\n";
        out_ << "  " << t("m") << " = and " << t("acc") << ", " << kSumMask << "\n";
        finish(t("m"), t("i5"), n);
    }

    void leafCall(const std::string &s, const std::string &i, std::size_t n, unsigned leafId) {
        out_ << "  " << t("arg") << " = and " << s << ", 1048575\n";
        out_ << "  " << t("r") << " = call @leaf" << leafId << "(" << t("arg") << ", " << i
             << ")\n";
        out_ << "  " << t("acc") << " = iadd.ovf " << s << ", " << t("r") << "\n";
        out_ << "  " << t("m") << " = and " << t("acc") << ", " << kSumMask << "\n";
        finish(t("m"), i, n);
    }

    void phiCycleLoop(const std::string &s, const std::string &i, std::size_t n) {
        // inner(p, q, k): rotate p and q every trip so the block parameters
        // form a phi cycle the edge-copy lowering must break correctly.
        const long long trips = pick(2, 6);
        out_ << "  " << t("q0") << " = and " << i << ", 4095\n";
        out_ << "  br " << l("in") << "(" << s << ", " << t("q0") << ", 0)\n\n";
        out_ << l("in") << "(" << t("p") << ": i64, " << t("q") << ": i64, " << t("k")
             << ": i64):\n";
        out_ << "  " << t("done") << " = scmp_ge " << t("k") << ", " << trips << "\n";
        out_ << "  cbr " << t("done") << ", " << l("ix") << "(" << t("p") << ", " << t("q") << "), "
             << l("is") << "(" << t("p") << ", " << t("q") << ", " << t("k") << ")\n\n";
        out_ << l("is") << "(" << t("p0") << ": i64, " << t("q0b") << ": i64, " << t("k0")
             << ": i64):\n";
        out_ << "  " << t("tq") << " = iadd.ovf " << t("q0b") << ", " << t("k0") << "\n";
        out_ << "  " << t("tp") << " = and " << t("p0") << ", " << kSumMask << "\n";
        out_ << "  " << t("k1") << " = iadd.ovf " << t("k0") << ", 1\n";
        out_ << "  br " << l("in") << "(" << t("tq") << ", " << t("tp") << ", " << t("k1")
             << ")\n\n";
        out_ << l("ix") << "(" << t("px") << ": i64, " << t("qx") << ": i64):\n";
        out_ << "  " << t("acc") << " = iadd.ovf " << t("px") << ", " << t("qx") << "\n";
        out_ << "  " << t("m") << " = and " << t("acc") << ", " << kSumMask << "\n";
        finish(t("m"), i, n);
    }

    void carriedFlag(const std::string &s, const std::string &i, std::size_t n, unsigned leafId) {
        // flag starts at `init` outside the inner loop and becomes `hit` on
        // the trips where a hash of (k, i) is zero mod 4; the loop reads it
        // only after it exits. The in-loop definition is a single constant,
        // so a post-allocation hoist that ignores header liveness would move
        // it into the preheader and erase `init`. A leaf call inside the loop
        // keeps the flag live across a call, which is what places it in a
        // callee-saved register (the only class the hoist considers).
        const long long trips = pick(3, 9);
        const long long init = pick(0, 1) == 0 ? 0 : pick(2, 99);
        const long long hit = init + pick(1, 50);
        out_ << "  br " << l("fh") << "(" << s << ", " << i << ", 0, " << init << ")\n\n";
        out_ << l("fh") << "(" << t("fs") << ": i64, " << t("fi") << ": i64, " << t("fk")
             << ": i64, " << t("fl") << ": i64):\n";
        out_ << "  " << t("fdone") << " = scmp_ge " << t("fk") << ", " << trips << "\n";
        out_ << "  cbr " << t("fdone") << ", " << l("fx") << "(" << t("fs") << ", " << t("fi")
             << ", " << t("fl") << "), " << l("fb") << "(" << t("fs") << ", " << t("fi") << ", "
             << t("fk") << ", " << t("fl") << ")\n\n";
        out_ << l("fb") << "(" << t("bs") << ": i64, " << t("bi") << ": i64, " << t("bk")
             << ": i64, " << t("bl") << ": i64):\n";
        out_ << "  " << t("ba") << " = and " << t("bs") << ", 1048575\n";
        out_ << "  " << t("br") << " = call @leaf" << leafId << "(" << t("ba") << ", " << t("bk")
             << ")\n";
        out_ << "  " << t("bh") << " = xor " << t("br") << ", " << t("bi") << "\n";
        out_ << "  " << t("bm") << " = and " << t("bh") << ", 3\n";
        out_ << "  " << t("bz") << " = icmp_eq " << t("bm") << ", 0\n";
        out_ << "  cbr " << t("bz") << ", " << l("fset") << "(" << t("bs") << ", " << t("bi")
             << ", " << t("bk") << "), " << l("fkeep") << "(" << t("bs") << ", " << t("bi") << ", "
             << t("bk") << ", " << t("bl") << ")\n\n";
        out_ << l("fset") << "(" << t("ss") << ": i64, " << t("si") << ": i64, " << t("sk")
             << ": i64):\n";
        out_ << "  br " << l("flat") << "(" << t("ss") << ", " << t("si") << ", " << t("sk") << ", "
             << hit << ")\n\n";
        out_ << l("fkeep") << "(" << t("ks") << ": i64, " << t("ki") << ": i64, " << t("kk")
             << ": i64, " << t("kl") << ": i64):\n";
        out_ << "  br " << l("flat") << "(" << t("ks") << ", " << t("ki") << ", " << t("kk") << ", "
             << t("kl") << ")\n\n";
        out_ << l("flat") << "(" << t("ls") << ": i64, " << t("li") << ": i64, " << t("lk")
             << ": i64, " << t("ll") << ": i64):\n";
        // A second call in the latch makes the flag call-crossing on the back
        // edge as well, so the allocator homes it in a callee-saved register
        // at the header: the register class the hoist considers.
        out_ << "  " << t("lc") << " = call @leaf" << leafId << "(" << t("ls") << ", " << t("lk")
             << ")\n";
        out_ << "  " << t("ls1") << " = and " << t("lc") << ", " << kSumMask << "\n";
        out_ << "  " << t("lk1") << " = iadd.ovf " << t("lk") << ", 1\n";
        out_ << "  br " << l("fh") << "(" << t("ls1") << ", " << t("li") << ", " << t("lk1") << ", "
             << t("ll") << ")\n\n";
        out_ << l("fx") << "(" << t("xs") << ": i64, " << t("xi") << ": i64, " << t("xl")
             << ": i64):\n";
        out_ << "  " << t("xw") << " = imul.ovf " << t("xl") << ", 977\n";
        out_ << "  " << t("acc") << " = iadd.ovf " << t("xs") << ", " << t("xw") << "\n";
        out_ << "  " << t("m") << " = and " << t("acc") << ", " << kSumMask << "\n";
        finish(t("m"), t("xi"), n);
    }

    void ehCatch(const std::string &s, const std::string &i, std::size_t n) {
        // A native EH frame around a division whose divisor is zero every
        // fourth trip. A handler may read only its own parameters, so the
        // stage state goes through the entry allocas: stored before the push,
        // reloaded in the recovery block the handler resumes into. The sum is
        // also read on the normal path after the pop, so it is live across
        // the frame push (the setjmp) and must be memory-homed by a backend
        // that honours EH-1.
        out_ << "  store i64, %eh_s, " << s << "\n";
        out_ << "  store i64, %eh_i, " << i << "\n";
        out_ << "  " << t("d") << " = and " << i << ", 3\n";
        out_ << "  " << t("num") << " = and " << s << ", 1048575\n";
        out_ << "  eh.push ^" << l("eh") << "\n";
        out_ << "  " << t("q") << " = sdiv.chk0 " << t("num") << ", " << t("d") << "\n";
        out_ << "  eh.pop\n";
        // The recovery block must postdominate the pushing block: the normal
        // path reaches it with the quotient plus the incoming sum (read after
        // the push), the handler with the trap kind.
        out_ << "  " << t("acc") << " = iadd.ovf " << s << ", " << t("q") << "\n";
        out_ << "  br " << l("rc") << "(" << t("acc") << ")\n\n";
        out_ << l("eh") << "(%err: Error, %tok: ResumeTok):\n";
        out_ << "  eh.entry\n";
        out_ << "  " << t("kind") << " = trap.kind %err\n";
        out_ << "  resume.label %tok, ^" << l("rc") << "(" << t("kind") << ")\n\n";
        out_ << l("rc") << "(" << t("rk") << ": i64):\n";
        out_ << "  " << t("rs") << " = load i64, %eh_s\n";
        out_ << "  " << t("ri") << " = load i64, %eh_i\n";
        out_ << "  " << t("hacc") << " = iadd.ovf " << t("rs") << ", " << t("rk") << "\n";
        out_ << "  " << t("hacc2") << " = iadd.ovf " << t("hacc") << ", " << pick(1, 500) << "\n";
        out_ << "  " << t("hm") << " = and " << t("hacc2") << ", " << kSumMask << "\n";
        finish(t("hm"), t("ri"), n);
    }

    void bitMix(const std::string &s, const std::string &i, std::size_t n) {
        out_ << "  " << t("h0") << " = imul.ovf " << i << ", 2654435761\n";
        out_ << "  " << t("h1") << " = lshr " << t("h0") << ", " << pick(3, 21) << "\n";
        out_ << "  " << t("h2") << " = xor " << t("h0") << ", " << t("h1") << "\n";
        out_ << "  " << t("h3") << " = shl " << t("h2") << ", " << pick(1, 5) << "\n";
        out_ << "  " << t("h4") << " = or " << t("h3") << ", " << s << "\n";
        out_ << "  " << t("h5") << " = and " << t("h4") << ", " << pick(4095, 1048575) << "\n";
        out_ << "  " << t("h6") << " = ashr " << t("h5") << ", " << pick(0, 3) << "\n";
        out_ << "  " << t("acc") << " = iadd.ovf " << s << ", " << t("h6") << "\n";
        out_ << "  " << t("m") << " = and " << t("acc") << ", " << kSumMask << "\n";
        finish(t("m"), i, n);
    }

  private:
    std::ostringstream &out_;
    std::mt19937_64 &rng_;
    std::size_t stage_;
};

/// @brief Emit a leaf helper `@leafN(%x, %k) -> i64` with a two-way diamond.
/// @brief Emit a self-recursive leaf `@leaf<id>(a, b)`: `b` steps down to
///        zero, adding a seeded increment to `a` on each step. The inliner
///        never inlines a recursive callee, so a call to it survives every
///        pipeline level and keeps the caller's values live across a call.
void emitRecursiveLeaf(std::ostringstream &out, std::mt19937_64 &rng, unsigned id) {
    std::uniform_int_distribution<long long> stepDist(1, 31);
    const long long step = stepDist(rng);
    out << "func @leaf" << id << "(i64 %a, i64 %b) -> i64 {\n";
    out << "entry(%a: i64, %b: i64):\n";
    out << "  %done = scmp_le %b, 0\n";
    out << "  cbr %done, base(%a), step(%a, %b)\n";
    out << "base(%r: i64):\n";
    out << "  %m = and %r, 1048575\n";
    out << "  ret %m\n";
    out << "step(%a1: i64, %b1: i64):\n";
    out << "  %a2 = iadd.ovf %a1, " << step << "\n";
    out << "  %b2 = isub.ovf %b1, 1\n";
    out << "  %r2 = call @leaf" << id << "(%a2, %b2)\n";
    out << "  ret %r2\n";
    out << "}\n\n";
}

void emitLeaf(std::ostringstream &out, std::mt19937_64 &rng, unsigned id) {
    std::uniform_int_distribution<long long> small(2, 9);
    std::uniform_int_distribution<long long> bit(0, 4);
    const std::string p = "@leaf" + std::to_string(id);
    out << "func " << p << "(%x: i64, %k: i64) -> i64 {\n";
    out << "entry(%x: i64, %k: i64):\n";
    out << "  %bit = and %k, " << (1LL << bit(rng)) << "\n";
    out << "  %odd = icmp_ne %bit, 0\n";
    out << "  cbr %odd, oddpath(%x, %k), evenpath(%x, %k)\n\n";
    out << "oddpath(%x0: i64, %k0: i64):\n";
    out << "  %t = imul.ovf %x0, " << small(rng) << "\n";
    out << "  %t1 = iadd.ovf %t, %k0\n";
    out << "  br merge(%t1)\n\n";
    out << "evenpath(%x2: i64, %k2: i64):\n";
    out << "  %t2 = shl %x2, " << bit(rng) + 1 << "\n";
    out << "  %t3 = isub.ovf %t2, %k2\n";
    out << "  br merge(%t3)\n\n";
    out << "merge(%m: i64):\n";
    out << "  %r = and %m, 1048575\n";
    out << "  ret %r\n";
    out << "}\n\n";
}

} // namespace

/// @copydoc kernelShapeName
const char *kernelShapeName(KernelShape shape) noexcept {
    switch (shape) {
        case KernelShape::CheckedArith:
            return "checked-arith";
        case KernelShape::IdxChkReuse:
            return "idx-chk-reuse";
        case KernelShape::DivRemConst:
            return "div-rem-const";
        case KernelShape::SwitchDispatch:
            return "switch-dispatch";
        case KernelShape::SelectDiamond:
            return "select-diamond";
        case KernelShape::LeafCall:
            return "leaf-call";
        case KernelShape::PhiCycleLoop:
            return "phi-cycle-loop";
        case KernelShape::BitMix:
            return "bit-mix";
        case KernelShape::EhCatch:
            return "eh-catch";
        case KernelShape::CarriedFlag:
            return "carried-flag";
        case KernelShape::Count:
            break;
    }
    return "?";
}

/// @copydoc generateKernelProgram
KernelProgram generateKernelProgram(std::uint64_t seed) {
    KernelProgram program;
    program.seed = seed;
    std::mt19937_64 rng(seed);

    std::uniform_int_distribution<std::uint64_t> tripDist(64, 512);
    std::uniform_int_distribution<std::size_t> stageDist(2, 4);
    std::uniform_int_distribution<unsigned> shapeDist(
        0, static_cast<unsigned>(KernelShape::Count) - 1);

    program.iterations = tripDist(rng);
    const std::size_t stageCount = stageDist(rng);
    for (std::size_t k = 0; k < stageCount; ++k)
        program.shapes.push_back(static_cast<KernelShape>(shapeDist(rng)));

    std::ostringstream out;
    out << "il 0.3.0\n\n";
    out << "// Generated IL kernel, seed " << seed << "\n\n";

    // One leaf helper per LeafCall stage, emitted before main.
    unsigned leafCount = 0;
    for (std::size_t k = 0; k < stageCount; ++k) {
        if (program.shapes[k] == KernelShape::LeafCall)
            emitLeaf(out, rng, leafCount++);
        else if (program.shapes[k] == KernelShape::CarriedFlag)
            emitRecursiveLeaf(out, rng, leafCount++);
    }

    out << "func @main() -> i64 {\n";
    out << "entry:\n";
    // The EH shape carries the stage state across its handler through memory.
    bool hasEh = false;
    for (KernelShape shape : program.shapes)
        hasEh = hasEh || shape == KernelShape::EhCatch;
    if (hasEh) {
        out << "  %eh_s = alloca 8\n";
        out << "  %eh_i = alloca 8\n";
    }
    out << "  br loop(0, 0)\n\n";
    out << "loop(%sum: i64, %i: i64):\n";
    out << "  %done = scmp_ge %i, " << program.iterations << "\n";
    out << "  cbr %done, exit(%sum), st_0(%sum, %i)\n\n";

    unsigned leafId = 0;
    for (std::size_t k = 0; k < stageCount; ++k) {
        StageWriter w(out, rng, k);
        const std::string s = "%s_in_" + std::to_string(k);
        const std::string i = "%i_in_" + std::to_string(k);
        out << "st_" << k << "(" << s << ": i64, " << i << ": i64):\n";
        switch (program.shapes[k]) {
            case KernelShape::CheckedArith:
                w.checkedArith(s, i, stageCount);
                break;
            case KernelShape::IdxChkReuse:
                w.idxChkReuse(s, i, stageCount);
                break;
            case KernelShape::DivRemConst:
                w.divRemConst(s, i, stageCount);
                break;
            case KernelShape::SwitchDispatch:
                w.switchDispatch(s, i, stageCount);
                break;
            case KernelShape::SelectDiamond:
                w.selectDiamond(s, i, stageCount);
                break;
            case KernelShape::LeafCall:
                w.leafCall(s, i, stageCount, leafId++);
                break;
            case KernelShape::PhiCycleLoop:
                w.phiCycleLoop(s, i, stageCount);
                break;
            case KernelShape::EhCatch:
                w.ehCatch(s, i, stageCount);
                break;
            case KernelShape::CarriedFlag:
                w.carriedFlag(s, i, stageCount, leafId++);
                break;
            case KernelShape::BitMix:
            case KernelShape::Count:
                w.bitMix(s, i, stageCount);
                break;
        }
    }

    out << "tail(%s_t: i64, %i_t: i64):\n";
    out << "  %masked = and %s_t, " << kSumMask << "\n";
    out << "  %next_i = iadd.ovf %i_t, 1\n";
    out << "  br loop(%masked, %next_i)\n\n";
    out << "exit(%result: i64):\n";
    out << "  %r = and %result, 255\n";
    out << "  ret %r\n";
    out << "}\n";

    program.il = out.str();
    return program;
}

} // namespace zanna::tests
