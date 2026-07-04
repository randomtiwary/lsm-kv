#pragma once

// Debug-only assertions for internal invariants.
//
// Enabled exclusively when CMake configures the Debug build type, which defines
// LSMKV_ENABLE_DCHECKS=1 on our targets. Release / RelWithDebInfo / MinSizeRel
// (and undecorated builds) compile these macros to nothing — independent of
// whether NDEBUG is set and without calling into assert().

#if defined(LSMKV_ENABLE_DCHECKS) && LSMKV_ENABLE_DCHECKS

#include <cstdio>
#include <cstdlib>

namespace lsmkv {
namespace debug_detail {

[[noreturn]] inline void CheckFail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "LSMKV_DCHECK failed: %s (%s:%d)\n", expr, file, line);
    std::abort();
}

}  // namespace debug_detail
}  // namespace lsmkv

#define LSMKV_DCHECKS_ENABLED 1
#define LSMKV_DCHECK(cond) \
    do { \
        if (!(cond)) { \
            ::lsmkv::debug_detail::CheckFail(#cond, __FILE__, __LINE__); \
        } \
    } while (0)
#define LSMKV_DCHECK_EQ(a, b) LSMKV_DCHECK((a) == (b))
#define LSMKV_DCHECK_NE(a, b) LSMKV_DCHECK((a) != (b))

#else

#define LSMKV_DCHECKS_ENABLED 0
#define LSMKV_DCHECK(cond) ((void)0)
#define LSMKV_DCHECK_EQ(a, b) ((void)0)
#define LSMKV_DCHECK_NE(a, b) ((void)0)

#endif
