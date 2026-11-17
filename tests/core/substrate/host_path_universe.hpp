#pragma once

// ════ THE TYPED HOST PATH HOME'S UNIVERSE, GENERATED FROM ITS OWN VOCABULARY (P69 H6c2, T5) ════
//
// ★★★ WHY GENERATED. The walk-up class opened five times, each time at a cell no hand-written table held: the
// pinned universe was whatever tables a lane wrote. Here it is the PRODUCT of the home's own vocabulary --
// the origins (`HostPath::Origin`), the uses (`core::Use`), the root SHAPES the parse answers (`DSS_ROOT_SHAPES`),
// the name EDGES the reference rule tells apart, the POSITION of the name, and (for a drive-relative spelling) the
// DRIVE -- pruned by the rules below, each a property of the axes and never an operation's own choice. One gtest case
// per cell, its name printing its axes, so an arm's C and G rows name CELLS and an undeclared red is seen.
//
// THE PRUNING RULES (the only ones):
//   (1) the shapes of the HOST's grammar (`kHostPathGrammar`): a spelling is read by the host it runs on;
//   (2) a shape that spells its VOLUME ALONE (bare, a root) carries no name: edge Plain, position Last -- and only a
//       GIVEN or an ANSWERED value is one (nothing lists or composes a volume);
//   (3) a NAMED DEVICE (`\\.\PIPE`) holds no file a cell can plant: GIVEN, edge Plain, position Last; the device a cell
//       names is the null device (`NUL`), which every Win32 host has;
//   (4) a STREAM is a file's: Leaf, Last -- never LISTED (a walk lists no stream);
//   (5) a LINK is a directory in the MIDDLE -- never LISTED (a walk never descends a link);
//   (6) the OTHER drive only for the drive-relative shapes (T11) -- and a SHORT NAME's is another LETTER: a drive
//       letter onto the scratch's own volume, which claims what the letter decides (its own current directory, the
//       alias read through it), never the volume's (whether a volume makes short names is its own policy, which
//       no test sets).
//
// THE STRUCTURAL RULE (the re-audit's T5): the REFERENCE rule is pinned over the whole product once
// (`NamesTheObjectTheHostReads`); every operation that hands the host a reference (`takesAReference` of its family in
// `DSS_HOST_PATH_OPERATIONS`) runs over the WITNESS cells -- where a value's reference and its text name different
// objects (a trailing period or space, a reserved stem, a short name, a stream, a link, the bare drive, a bare volume)
// -- and is judged by the object it touched (`EveryOperationTouchesTheObjectTheHostReads`), and every one of them runs
// on every cell without throwing (`IsTotal`).

#include "core/substrate/host_path.hpp"
#include "core/substrate/path_identity.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace dss::test_support::universe {

enum class Origin : std::uint8_t { Given, Listed, Composed, Answered };
enum class Edge : std::uint8_t {
    Plain, TrailingPeriod, TrailingSpace, ReservedStem, ShortName, Stream, Link,
    OutsideCodePage,     // a name no ANSI code page holds (U+1F600): every rendering must stay total (T4)
    UnpairedSurrogate,   // Win32: a name with a lone UTF-16 surrogate, which NTFS stores and UTF-8 cannot spell
    NotUtf8,             // POSIX: a name whose bytes are not UTF-8
};
enum class Position : std::uint8_t { Middle, Last };
enum class Drive : std::uint8_t { Same, Other, OtherLetter };

// What a shape spells: a NAME below its volume (the edges apply to it), its VOLUME ALONE, or a NAMED DEVICE.
enum class Form : std::uint8_t { Named, VolumeAlone, NamedDevice };

[[nodiscard]] constexpr Form formOf(core::RootShape shape) noexcept {
    using S = core::RootShape;
    switch (shape) {
        case S::Relative:
        case S::DriveRelativeName:
        case S::Rooted:
        case S::Drive:
        case S::ShareName:
        case S::DeviceDrive:
        case S::DeviceVolume:
        case S::DeviceUnc:
        case S::DeviceGlobalRoot:
        case S::VerbatimDrive:
        case S::VerbatimVolume:
        case S::VerbatimUnc:
        case S::VerbatimGlobalRoot:
        case S::NtDrive:
        case S::NtVolume:
        case S::NtUnc:
        case S::NtGlobalRoot:
        case S::PosixRelative:
        case S::PosixRooted:
        case S::PosixAuthority: return Form::Named;
        case S::DriveRelativeBare:
        case S::RootedBare:
        case S::DriveRoot:
        case S::ShareServer:
        case S::ShareBare:
        case S::ShareRoot:
        case S::DeviceDriveBare:
        case S::DeviceVolumeBare:
        case S::DeviceUncBare:
        case S::DeviceGlobalRootBare:
        case S::VerbatimDriveBare:
        case S::VerbatimVolumeBare:
        case S::VerbatimUncBare:
        case S::VerbatimGlobalRootBare:
        case S::NtDriveBare:
        case S::NtVolumeBare:
        case S::NtUncBare:
        case S::NtGlobalRootBare:
        case S::PosixRoot:
        case S::PosixAuthorityRoot: return Form::VolumeAlone;
        case S::DeviceNamedBare:
        case S::DeviceNamed:
        case S::VerbatimNamedBare:
        case S::VerbatimNamed:
        case S::NtNamedBare:
        case S::NtNamed: return Form::NamedDevice;
    }
    return Form::Named;   // unreachable: the switch names every row
}

[[nodiscard]] constexpr bool isDriveRelative(core::RootShape shape) noexcept {
    return shape == core::RootShape::DriveRelativeBare || shape == core::RootShape::DriveRelativeName;
}

[[nodiscard]] constexpr char const* nameOf(Origin o) noexcept {
    switch (o) {
        case Origin::Given: return "Given";
        case Origin::Listed: return "Listed";
        case Origin::Composed: return "Composed";
        case Origin::Answered: return "Answered";
    }
    return "?";
}
[[nodiscard]] constexpr char const* nameOf(core::Use u) noexcept {
    switch (u) {
        case core::Use::Leaf: return "Leaf";
        case core::Use::Container: return "Container";
    }
    return "?";
}
[[nodiscard]] constexpr char const* nameOf(Edge e) noexcept {
    switch (e) {
        case Edge::Plain: return "Plain";
        case Edge::TrailingPeriod: return "TrailingPeriod";
        case Edge::TrailingSpace: return "TrailingSpace";
        case Edge::ReservedStem: return "ReservedStem";
        case Edge::ShortName: return "ShortName";
        case Edge::Stream: return "Stream";
        case Edge::Link: return "Link";
        case Edge::OutsideCodePage: return "OutsideCodePage";
        case Edge::UnpairedSurrogate: return "UnpairedSurrogate";
        case Edge::NotUtf8: return "NotUtf8";
    }
    return "?";
}
[[nodiscard]] constexpr char const* nameOf(Position p) noexcept {
    switch (p) {
        case Position::Middle: return "Middle";
        case Position::Last: return "Last";
    }
    return "?";
}
[[nodiscard]] constexpr char const* nameOf(Drive d) noexcept {
    switch (d) {
        case Drive::Same: return "SameDrive";
        case Drive::Other: return "OtherDrive";
        case Drive::OtherLetter: return "OtherLetter";
    }
    return "?";
}

// The edges a grammar's reference rule tells apart: a POSIX name is resolved by no position (every edge but a link is
// a literal name there, kept to pin that it IS one), and POSIX has no short names and no streams. The TEXT edges are
// each grammar's names that no narrow rendering spells: Win32's lone surrogate, POSIX's bytes that are not UTF-8.
[[nodiscard]] inline std::vector<Edge> edgesOf(core::PathGrammar grammar) {
    if (grammar == core::PathGrammar::Win32)
        return {Edge::Plain,     Edge::TrailingPeriod, Edge::TrailingSpace, Edge::ReservedStem,
                Edge::ShortName, Edge::Stream,         Edge::Link,          Edge::OutsideCodePage,
                Edge::UnpairedSurrogate};
    return {Edge::Plain, Edge::TrailingPeriod, Edge::TrailingSpace, Edge::ReservedStem, Edge::Link,
            Edge::OutsideCodePage, Edge::NotUtf8};
}

struct Cell {
    Origin          origin;
    core::Use       use;
    core::RootShape shape;
    Edge            edge;
    Position        position;
    Drive           drive;
};

// `Given_Container_DriveRelativeBare_Plain_Last_SameDrive`: every axis, in order.
[[nodiscard]] inline std::string cellName(Cell const& c) {
    return std::string{nameOf(c.origin)} + "_" + nameOf(c.use) + "_" + core::shapeName(c.shape) + "_" + nameOf(c.edge)
         + "_" + nameOf(c.position) + "_" + nameOf(c.drive);
}

// Whether a cell is a WITNESS: its value's reference and its text name different objects under some reading -- the
// cells every reference-taking operation is judged on.
[[nodiscard]] constexpr bool isWitness(Cell const& c) noexcept {
    if (c.edge != Edge::Plain) return true;
    switch (c.shape) {
        case core::RootShape::DriveRelativeBare:
        case core::RootShape::VerbatimDriveBare:
        case core::RootShape::VerbatimVolumeBare:
        case core::RootShape::VerbatimUncBare:
        case core::RootShape::VerbatimGlobalRootBare:
        case core::RootShape::NtDriveBare:
        case core::RootShape::NtVolumeBare:
        case core::RootShape::NtUncBare:
        case core::RootShape::NtGlobalRootBare: return true;
        default: return false;   // the rest: a plain name, text and reference one object
    }
}

// THE PRODUCT, pruned by the rules (1)-(6) above alone.
[[nodiscard]] inline std::vector<Cell> generatedCells() {
    std::vector<Cell> out;
    constexpr Origin    origins[]   = {Origin::Given, Origin::Listed, Origin::Composed, Origin::Answered};
    constexpr core::Use uses[]      = {core::Use::Leaf, core::Use::Container};
    constexpr Position  positions[] = {Position::Middle, Position::Last};
    for (core::RootShapeRow const& row : core::kRootShapes) {
        if (row.grammar != core::kHostPathGrammar) continue;                                         // (1)
        Form const               form   = formOf(row.shape);
        std::vector<Drive> const drives = isDriveRelative(row.shape) ? std::vector<Drive>{Drive::Same, Drive::Other}
                                                                     : std::vector<Drive>{Drive::Same};   // (6)
        for (Drive const drive : drives) {
            if (form != Form::Named) {
                for (Origin const origin : origins) {
                    if (form == Form::NamedDevice && origin != Origin::Given) continue;             // (3)
                    if (form == Form::VolumeAlone && origin != Origin::Given && origin != Origin::Answered)
                        continue;                                                                    // (2)
                    for (core::Use const use : uses) out.push_back({origin, use, row.shape, Edge::Plain, Position::Last, drive});
                }
                continue;
            }
            for (Edge const edge : edgesOf(row.grammar)) {
                for (Origin const origin : origins) {
                    if ((edge == Edge::Stream || edge == Edge::Link) && origin == Origin::Listed) continue;   // (4)(5)
                    for (core::Use const use : uses) {
                        if (edge == Edge::Stream && use != core::Use::Leaf) continue;                // (4)
                        for (Position const position : positions) {
                            if (edge == Edge::Stream && position != Position::Last) continue;        // (4)
                            if (edge == Edge::Link && position != Position::Middle) continue;        // (5)
                            Drive const at = drive == Drive::Other && edge == Edge::ShortName ? Drive::OtherLetter
                                                                                               : drive;   // (6)
                            out.push_back({origin, use, row.shape, edge, position, at});
                        }
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace dss::test_support::universe
