#include "parse_surface_int.h"

#include <cstdlib>
#include <iostream>
#include <string>

using OHOS::Media::ParseSurfaceInt;

static int g_fail = 0;

static void ExpectTrue(const char *name, bool ok)
{
    if (!ok) {
        std::cerr << "FAIL " << name << "\n";
        ++g_fail;
    }
}

static void ExpectFalse(const char *name, bool ok)
{
    ExpectTrue(name, !ok);
}

static void ExpectEq(const char *name, int32_t got, int32_t want)
{
    if (got != want) {
        std::cerr << "FAIL " << name << " got=" << got << " want=" << want << "\n";
        ++g_fail;
    }
}

int main()
{
    int32_t out = -999;
    ExpectTrue("0", ParseSurfaceInt("0", out));
    ExpectEq("0val", out, 0);
    ExpectTrue("123", ParseSurfaceInt("123", out));
    ExpectEq("123val", out, 123);
    ExpectTrue("-1", ParseSurfaceInt("-1", out));
    ExpectEq("-1val", out, -1);
    ExpectTrue("INT_MAX", ParseSurfaceInt("2147483647", out));
    ExpectEq("INT_MAXval", out, 2147483647);

    ExpectFalse("empty", ParseSurfaceInt("", out));
    ExpectFalse("abc", ParseSurfaceInt("abc", out));
    ExpectFalse("12a", ParseSurfaceInt("12a", out));
    ExpectFalse("space", ParseSurfaceInt(" 12", out));
    ExpectFalse("overflow", ParseSurfaceInt("2147483648", out));
    ExpectFalse("huge", ParseSurfaceInt("9999999999999999999", out));
    ExpectFalse("neg_overflow", ParseSurfaceInt("-2147483649", out));

    // Prove bare stoi would throw on overflow while helper stays safe
    bool threw = false;
    try {
        (void)std::stoi(std::string("2147483648"));
    } catch (...) {
        threw = true;
    }
    ExpectTrue("stoi_overflow_throws", threw);

    if (g_fail != 0) {
        std::cerr << g_fail << " failures\n";
        return 1;
    }
    std::cout << "camera_lite ParseSurfaceInt host test passed\n";
    return 0;
}
