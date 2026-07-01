#include <doctest/doctest.h>
#include "../Runtime/Framework/AudioSubsystem.h"
#include "../Runtime/Framework/AppState.h"
#include "../Runtime/Framework/ProjectConfig.h"

TEST_CASE("AudioSubsystem initialization and settings") {
    AppState state;
    // Set up a mock project path so it doesn't fail trying to read missing dirs if possible
    state.projectConfig.setProjectPath(".");

    AudioSubsystem audio(state);

    SUBCASE("Subsystem metadata is correct") {
        CHECK(audio.name() == "AudioSubsystem");
        CHECK(audio.phase() == SubsystemPhase::Foundation);
        
        auto deps = audio.dependencies();
        REQUIRE(deps.size() == 1);
        CHECK(deps[0] == "Window");
    }

    SUBCASE("Default settings are correctly configured") {
        auto& settings = audio.settings();
        CHECK(settings.enabled == true);
        CHECK(settings.mute == false);
        CHECK(settings.masterVolume == 1.0f);
        CHECK(settings.ambientEnabled == true);
        CHECK(settings.footstepsEnabled == true);
    }

    SUBCASE("Modifying settings works correctly") {
        auto& settings = audio.settings();
        settings.masterVolume = 0.5f;
        settings.mute = true;

        CHECK(audio.settings().masterVolume == doctest::Approx(0.5f));
        CHECK(audio.settings().mute == true);
    }
}
