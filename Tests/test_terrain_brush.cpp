#include <doctest/doctest.h>
#include "../Engine/Terrain/TerrainBrushSettings.h"

TEST_CASE("TerrainBrushSettings initialization") {
    TerrainBrushSettings brush;

    SUBCASE("Default settings are correctly configured") {
        CHECK(brush.enabled == false);
        CHECK(brush.mode == 0);
        CHECK(brush.target == 0);
        CHECK(brush.radius == doctest::Approx(6.0f));
        CHECK(brush.strength == doctest::Approx(2.0f));
        CHECK(brush.scatterCount == 6);
    }

    SUBCASE("Modifying settings works correctly") {
        brush.enabled = true;
        brush.mode = 2;
        brush.radius = 10.0f;

        CHECK(brush.enabled == true);
        CHECK(brush.mode == 2);
        CHECK(brush.radius == doctest::Approx(10.0f));
    }
}
