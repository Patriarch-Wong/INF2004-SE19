#pragma once

#include <cstdio>

// Minimal check helper shared by the host test programs.
namespace test_check
{
inline int &failures()
{
    static int count = 0;
    return count;
}

inline void check(bool condition, const char *description)
{
    if (!condition)
    {
        ++failures();
        std::printf("FAIL: %s\n", description);
    }
}

// Prints the summary line and returns the process exit code.
inline int finish(const char *suite)
{
    if (failures() == 0)
    {
        std::printf("All %s host tests passed\n", suite);
        return 0;
    }
    std::printf("%d %s check(s) failed\n", failures(), suite);
    return 1;
}
}
