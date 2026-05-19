#include "core/types/lexer_mode.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

namespace {

[[noreturn]] void lexerModeFatal(char const* msg) noexcept {
    std::fputs("dss::LexerModeStack fatal: ", stderr);
    std::fputs(msg, stderr);
    std::fputs("\n", stderr);
    std::abort();
}

} // namespace

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
    if (frames_.empty()) lexerModeFatal("pop() on empty stack");
    frames_.pop_back();
}

void LexerModeStack::replaceTop(LexerModeId mode) {
    if (frames_.empty()) lexerModeFatal("replaceTop() on empty stack");
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
    if (frames_.empty()) lexerModeFatal("top() on empty stack");
    return frames_.back();
}

LexerModeStack::Snapshot LexerModeStack::snapshot() const {
    Snapshot s;
    s.frames_ = frames_;
    s.owner_  = reinterpret_cast<std::uintptr_t>(this);
    return s;
}

void LexerModeStack::restore(Snapshot const& snap) {
    if (snap.owner_ != reinterpret_cast<std::uintptr_t>(this)) {
        lexerModeFatal("restore() with a snapshot from a different stack");
    }
    frames_ = snap.frames_;
}

} // namespace dss
