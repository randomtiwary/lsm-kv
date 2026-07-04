
#include "test_harness.h"
#include "lsmkv/debug.h"

// Confirms the macro is wired to the active CMake config: enabled in Debug,
// compiled out otherwise. Also exercises LSMKV_DCHECK on a passing condition.
TEST(debug_dcheck_config) {
#if defined(LSMKV_ENABLE_DCHECKS) && LSMKV_ENABLE_DCHECKS
    expect(LSMKV_DCHECKS_ENABLED == 1, "dchecks on in Debug");
#else
    expect(LSMKV_DCHECKS_ENABLED == 0, "dchecks off outside Debug");
#endif
    LSMKV_DCHECK(true);
    LSMKV_DCHECK_EQ(1, 1);
    LSMKV_DCHECK_NE(0, 1);
    expect(true, "dcheck macros callable");
}
