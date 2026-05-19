#include "core/types/lexer_mode.hpp"

#include <utility>

namespace dss {

std::string_view modeOpName(ModeOp op) noexcept {
    switch (op) {
        case ModeOp::None:        return "none";
        case ModeOp::PushMode:    return "pushMode";
        case ModeOp::PopMode:     return "popMode";
        case ModeOp::ReplaceMode: return "replaceMode";
    }
    return "none";   // unreachable; satisfies non-exhaustive compilers
}

void LexerModeStack::push(LexerModeId mode) {
    frames_.push_back(mode);
}

void LexerModeStack::pop() {
    if (!frames_.empty()) frames_.pop_back();
}

void LexerModeStack::replaceTop(LexerModeId mode) {
    if (frames_.empty()) return;
    frames_.back() = mode;
}

void LexerModeStack::apply(ModeOp op, LexerModeId arg) {
    switch (op) {
        case ModeOp::None:        break;
        case ModeOp::PushMode:    push(arg);            break;
        case ModeOp::PopMode:     pop();                break;
        case ModeOp::ReplaceMode: replaceTop(arg);      break;
    }
}

LexerModeId LexerModeStack::top() const noexcept {
    return frames_.empty() ? LexerModeId{} : frames_.back();
}

LexerModeStack::Snapshot LexerModeStack::snapshot() const {
    Snapshot s;
    s.frames_ = frames_;
    return s;
}

void LexerModeStack::restore(Snapshot const& snap) {
    frames_ = snap.frames_;
}

} // namespace dss
