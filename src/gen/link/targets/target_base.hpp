#pragma once

#include "core/export.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace dss {

enum class Endianness { Little, Big };

/// Abstract base class for all target platform emitters.
/// Each target knows how to produce a binary for a specific OS + architecture.
class DSS_EXPORT TargetBase {
public:
    virtual ~TargetBase() = default;

    /// Human-readable target name (e.g. "windows-x86_64").
    virtual std::string name() const = 0;

    /// File extension for this target's output (e.g. ".exe", "", ".wasm").
    virtual std::string outputExtension() const = 0;

    /// Pointer size in bytes (4 for 32-bit, 8 for 64-bit).
    virtual size_t pointerSize() const = 0;

    /// Target byte order.
    virtual Endianness endianness() const = 0;
};

} // namespace dss
