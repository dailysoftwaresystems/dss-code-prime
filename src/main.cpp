#include "program/cli_args.hpp"
#include "program/dump_predefined_macros.hpp"
#include "program/program.hpp"

#include <iostream>

int main(int argc, char* argv[]) {
    // ── `--dump-predefined-macros`: answered HERE, ahead of the compiler ─────
    //
    // The flag asks what an EMPTY translation unit would see for the resolved
    // (language x target x object-format) triple and compiles nothing, so it
    // needs no `Program` at all — no output dir, no reporter policy, no CU
    // build. Intercepting it at the entry point is what "exits without
    // compiling" means structurally rather than by convention.
    //
    // On a PARSE FAILURE this branch does not fire and `Program::run` re-parses
    // and prints the canonical `error: <detail>` + help. That keeps ONE owner of
    // CLI-error presentation; `parseCliArgs` is pure, so the second parse costs
    // one argv walk and cannot disagree with the first.
    //
    // `helpMode` is tested FIRST for the same reason `Program::run` tests it
    // first: `--help` alongside any mode flag prints help and exits 0, and the
    // parser lets that pair through (its tail returns early on `helpMode`
    // WITHOUT requiring --language/--target), so a dump branch that ignored it
    // would run with an unvalidated, empty language.
    {
        auto const parsed = dss::parseCliArgs(argc, argv);
        if (parsed && !parsed->helpMode && parsed->dumpPredefinedMacros) {
            return dss::dumpPredefinedMacros(*parsed, std::cout, std::cerr);
        }
    }

    dss::Program program;
    return program.run(argc, argv);
}
