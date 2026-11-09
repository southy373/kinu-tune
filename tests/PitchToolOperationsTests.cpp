// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#include "../Source/Utils/PitchToolOperations.h"
#include "TestAssert.h"
#include <cmath>
#include <iostream>
#include <vector>

// Batch 2 (B7): straighten (vibrato scaling) restricted to a sub-range of
// the segment. Inside the range the factor applies in full; outside it the
// curve is untouched, with a short feather at the boundaries.
namespace
{
bool near(float a, float b, float tolerance = 1.0e-4f)
{
    return std::abs(a - b) <= tolerance;
}
} // namespace

int main()
{
    using namespace PitchToolOperations;

    std::vector<float> curve(101, 0.0f);
    for (size_t i = 0; i < curve.size(); ++i)
        curve[i] = std::sin(static_cast<float>(i) * 0.2f);

    // Flatten (factor 0) inside [0.25, 0.75]: the middle is flat, the ends
    // keep the original curve, and the feather keeps them close nearby.
    {
        const auto out = scaleVibratoInRange(curve, 0.0f, 0.25f, 0.75f);
        CHECK(out.size() == curve.size());
        CHECK(near(out[50], 0.0f));            // centre: fully flattened
        CHECK(near(out[0], curve[0]));         // far outside: untouched
        CHECK(near(out[100], curve[100]));
        CHECK(std::abs(out[20]) > std::abs(out[25])); // feather ramps in
        CHECK(std::abs(out[25]) < std::abs(curve[25]));
    }

    // Amplify (factor 2) inside the range doubles the deviation there only.
    {
        const auto out = scaleVibratoInRange(curve, 2.0f, 0.25f, 0.75f);
        CHECK(near(out[50], curve[50] * 2.0f));
        CHECK(near(out[0], curve[0]));
    }

    // A full range is exactly the plain scaling.
    {
        const auto ranged = scaleVibratoInRange(curve, 0.3f, 0.0f, 1.0f);
        const auto plain = scaleVibrato(curve, 0.3f);
        for (size_t i = 0; i < curve.size(); ++i)
            CHECK(near(ranged[i], plain[i]));
    }

    // applyAllTransformations: the range parameters route the vibrato
    // factor through the ranged scaling; the defaults keep the old result.
    {
        const auto full = applyAllTransformations(curve, 0.0f, 0.0f, 0.0f, 0, 0);
        for (float v : full)
            CHECK(near(v, 0.0f));
        const auto ranged = applyAllTransformations(curve, 0.0f, 0.0f, 0.0f, 0, 0,
                                                    {}, 1.0f, 0.25f, 0.75f);
        CHECK(near(ranged[50], 0.0f));
        CHECK(near(ranged[0], curve[0]));
        CHECK(near(ranged[100], curve[100]));
    }

    std::cout << "Pitch tool operations tests passed\n";
    return 0;
}
