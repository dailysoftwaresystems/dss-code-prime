// A UNIT BUILT TWICE (P69 f26e, B1 A4; tests/link/CMakeLists.txt registers both): a link pass answers a
// `LinkPassOutcome` (src/link/link_pass_outcome.hpp), and a caller that drops it drops the refusal with it.
//   * built as is, it KEEPS the answer, and compiles;
//   * built with DSS_DISCARD_THE_LINK_PASS_OUTCOME, it DROPS the answer, and must not compile: the type is
//     `[[nodiscard]]`, and the root CMakeLists makes the discard an error on every compiler. The test
//     `link/a_discarded_link_pass_outcome_does_not_compile` builds that twin and reads the compiler's error.
#include "link/link_pass_outcome.hpp"

namespace {

dss::linker::LinkPassOutcome aPassThatRefuses() {
    return dss::linker::LinkPassOutcome::Refused;
}

}  // namespace

int dssLinkPassOutcomeFixture();

int dssLinkPassOutcomeFixture() {
#if defined(DSS_DISCARD_THE_LINK_PASS_OUTCOME)
    aPassThatRefuses();   // the refusal dropped: an ERROR on every compiler
    return 0;
#else
    return aPassThatRefuses() == dss::linker::LinkPassOutcome::Refused ? 0 : 1;
#endif
}
