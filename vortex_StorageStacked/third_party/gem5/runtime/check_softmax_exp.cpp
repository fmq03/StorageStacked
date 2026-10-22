#include <cmath>
#include <cstdio>
#include "../../vortex/sdk/device_math.h"

int main() {
    double max_relative = 0, max_absolute = 0;
    unsigned errors = 0;
    for (unsigned i = 0; i <= 1000000; ++i) {
        const float x = -100.0f * i / 1000000;
        const double reference = std::exp(double(x));
        const double actual = device_exp(x);
        const double absolute = std::abs(actual - reference);
        max_absolute = std::fmax(max_absolute, absolute);
        if (x >= -80.0f)
            max_relative = std::fmax(max_relative, absolute / reference);
        errors += !std::isfinite(actual) || actual < 0 ||
                  absolute > 1e-6 * reference + 2e-35;
    }
    std::printf("{\"samples\":1000001,\"max_abs_error\":%.12g,"
                "\"max_relative_error_above_cutoff\":%.12g,\"errors\":%u,"
                "\"passed\":%s}\n",
                max_absolute, max_relative, errors, errors == 0 ? "true" : "false");
    return errors != 0;
}
