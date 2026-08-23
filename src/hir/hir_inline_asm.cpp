#include "hir/hir_inline_asm.hpp"

#include "core/types/target_schema.hpp"   // TargetSchema::asmConstraint (the letter half)

#include <cstdio>
#include <cstdlib>

namespace dss {

std::string_view
asmConstraintDefectDescription(HirAsmConstraintDefect d) noexcept {
    switch (d) {
        case HirAsmConstraintDefect::None:
            return "no defect";
        case HirAsmConstraintDefect::Empty:
            return "the constraint carries no machine letter — there is "
                   "nothing to bind the operand to";
        case HirAsmConstraintDefect::MultiAlternative:
            return "MULTI-ALTERNATIVE: comma-separated alternatives (GNU "
                   "6.47.2.4) ask the register allocator to CHOOSE a binding "
                   "rather than to honour one; write the single alternative "
                   "this statement needs";
        case HirAsmConstraintDefect::UnknownModifier:
            return "UNRECOGNISED MODIFIER: the only constraint modifiers this "
                   "front end owns are '=' (output), '+' (in-out), '&' "
                   "(earlyclobber) and '%' (commutative)";
        case HirAsmConstraintDefect::MisplacedOutputModifier:
            return "MISPLACED OUTPUT MODIFIER: '=' / '+' must appear once, "
                   "first — a second one contradicts the first rather than "
                   "repeating it";
    }
    return "unknown constraint defect";
}

HirAsmConstraintParse parseAsmConstraint(std::string_view raw) {
    HirAsmConstraintParse out;
    out.value.raw.assign(raw);

    // MULTI-ALTERNATIVE is decided FIRST and without looking at anything else.
    // The comma is grammar on every processor, so this arm needs no target and
    // must not be reachable only after some earlier arm happens not to fire —
    // `"=r,m"` and `"#r,m"` are both multi-alternative, and reporting the
    // second as an unknown modifier would send the author to the wrong fix.
    if (raw.find(',') != std::string_view::npos) {
        out.defect = HirAsmConstraintDefect::MultiAlternative;
        return out;
    }

    std::size_t i = 0;
    bool sawOutputModifier = false;
    // The MODIFIER RUN. It ends at the first character that is not one of the
    // four; everything from there on is the letter SPELLING, verbatim.
    while (i < raw.size()) {
        char const c = raw[i];
        if (c == '=' || c == '+') {
            if (sawOutputModifier) {
                out.defect = HirAsmConstraintDefect::MisplacedOutputModifier;
                return out;
            }
            // A `=`/`+` after a `&`/`%` is the same contradiction seen from the
            // other side: GNU writes the output modifier first, and a parser
            // that accepted `"&=r"` would be admitting a spelling no reference
            // compiler produces (the bidirectional half of the
            // reference-compilers-are-the-spec rule).
            if (i != 0) {
                out.defect = HirAsmConstraintDefect::MisplacedOutputModifier;
                return out;
            }
            sawOutputModifier    = true;
            out.value.isOutput    = true;
            out.value.isReadWrite = (c == '+');
            ++i;
            continue;
        }
        if (c == '&') { out.value.earlyClobber = true; ++i; continue; }
        if (c == '%') { out.value.commutative  = true; ++i; continue; }
        break;
    }

    if (i >= raw.size()) {
        out.defect = HirAsmConstraintDefect::Empty;
        return out;
    }

    // THE SPELLING. A modifier character reappearing inside it is a defect, not
    // a late flag: `"r&"` is not an earlyclobber `r`, it is a constraint this
    // front end cannot read, and absorbing the `&` into the letter would report
    // the miss as "letter 'r&' undeclared" — true, and useless.
    std::string_view const spelling = raw.substr(i);
    for (char const c : spelling) {
        if (c == '=' || c == '+') {
            out.defect = HirAsmConstraintDefect::MisplacedOutputModifier;
            return out;
        }
        if (c == '&' || c == '%') {
            out.defect = HirAsmConstraintDefect::UnknownModifier;
            return out;
        }
        // Everything a target may legally declare as a letter is alphanumeric
        // (`r`, `x`, `a`, `Ush`, `Yz`, and gcc's digit MATCHING constraints).
        // Anything else in leading position was a modifier we do not own — the
        // GNU cost markers `#`, `*`, `?`, `!` land here — and anything else in
        // trailing position is not a spelling either.
        bool const alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                           || (c >= 'A' && c <= 'Z');
        if (!alnum) {
            out.defect = HirAsmConstraintDefect::UnknownModifier;
            return out;
        }
    }

    out.value.letter.assign(spelling);
    return out;
}

bool asmConstraintLooksMultiLetter(TargetSchema const& target,
                                   std::string_view    spelling) {
    if (spelling.size() < 2)                       return false;
    if (target.asmConstraint(spelling) != nullptr) return false;
    // Every character must resolve on its own for the "two letters written
    // together" reading to be PROVEN. One unresolvable character and the
    // spelling is simply undeclared — reporting it as multi-letter would tell
    // the author to split a constraint that splitting cannot fix.
    for (char const c : spelling) {
        std::string_view const one{&c, 1};
        if (target.asmConstraint(one) == nullptr) return false;
    }
    return true;
}

std::uint32_t HirInlineAsmPool::add(HirInlineAsmDescriptor d) {
    pool_.push_back(std::move(d));
    // 1-BASED: the handle is the new size, so the first descriptor is handle 1
    // and `kNoInlineAsmDescriptor` (0) can never be produced.
    return static_cast<std::uint32_t>(pool_.size());
}

HirInlineAsmDescriptor const& HirInlineAsmPool::at(std::uint32_t handle) const {
    if (handle == kNoInlineAsmDescriptor || handle > pool_.size()) {
        std::fprintf(stderr,
                     "dss::HirInlineAsmPool fatal: handle %u does not resolve "
                     "(pool size %zu; 0 is the no-descriptor sentinel)\n",
                     handle, pool_.size());
        std::abort();
    }
    return pool_[handle - 1];
}

} // namespace dss
