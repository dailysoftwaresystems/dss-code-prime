#pragma once

// DLL export/import macros for cross-platform shared library support.
// When building the DLL: DSS_EXPORT marks symbols for export.
// When consuming the DLL: DSS_EXPORT marks symbols for import.
// When linking statically: DSS_EXPORT is a no-op.

#if defined(DSS_SHARED_BUILD)
    #if defined(_WIN32)
        #if defined(DSS_BUILDING_DLL)
            #define DSS_EXPORT __declspec(dllexport)
        #else
            #define DSS_EXPORT __declspec(dllimport)
        #endif
    #else
        #define DSS_EXPORT __attribute__((visibility("default")))
    #endif
#else
    #define DSS_EXPORT
#endif
