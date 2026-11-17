#pragma once

#include <cstdint>

// THE CONVENTION A TEST LOWERS UNDER WHEN IT NAMES NONE (P69 lane `cs`, fold f26d -- review-csf26c2 N8): the index of
// the target's FIRST declared calling convention, which a test's hand-built MIR was built for. `lowerToLir` takes
// the convention as a required argument, so every caller states the one it means; a test that means another
// convention passes that convention's index (the one its register allocation reads).
namespace dss::test {
inline constexpr std::uint16_t kFirstConventionOfTheTarget = 0;
} // namespace dss::test
