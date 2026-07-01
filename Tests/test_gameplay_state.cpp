#include <doctest/doctest.h>
#include "../Runtime/Framework/GameplayState.h"

TEST_CASE("GameplayState initialization") {
    GameplayState g;
    CHECK(g.playerId == 0);
    CHECK(g.woodCount == 0);
    CHECK(g.axeEntity == 0);
    CHECK(g.lastPlayerYaw == 0.0f);
    CHECK(g.lastPlayerPitch == 0.0f);
    CHECK(g.hasLastPlayerRot == false);
    CHECK(static_cast<int>(g.activeSlot) == 1); // HotbarSlot::Axe
    CHECK(g.torchEntity == 0);
}

TEST_CASE("ViewmodelSettings defaults") {
    ViewmodelSettings v;
    CHECK(v.axeEnabled == true);
    CHECK(v.axeOffset.x == doctest::Approx(0.06f));
    CHECK(v.axeScale.x == doctest::Approx(0.66f));
    CHECK(v.torchEnabled == true);
    CHECK(v.usePlayerCameraInEdit == true);
}

TEST_CASE("GrabState initialization") {
    GrabState gs;
    CHECK(gs.grabbedEntityId == 0);
    CHECK(gs.grabbedDistance == doctest::Approx(3.0f));
    CHECK(gs.grabbedHadRigidbody == false);
    CHECK(gs.grabbedReleased == false);
}

TEST_CASE("DebugGameplayOverlay initialization") {
    DebugGameplayOverlay d;
    CHECK(d.debugMouseDX == 0.0f);
    CHECK(d.debugGameplayHit == false);
    CHECK(d.debugGrabInstance == -1);
}
