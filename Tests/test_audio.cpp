// Exercise the same decoder/voice code as the runtime, without an output device.
// Mixing real asset PCM catches bad Vorbis integration that scheduler tests miss.
#if defined(_WIN32)
#include <windows.h>
#endif
#include <doctest/doctest.h>
#include "subsystems/FootstepAudio.h"
#include "subsystems/AudioSettings.h"
#include <fstream>
#include <limits>
#include <memory>
#pragma push_macro("CHECK")
#undef CHECK
#include "stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "subsystems/AudioVoice.h"
#pragma pop_macro("CHECK") // stb_vorbis defines CHECK as a no-op internally.

namespace {
const auto assetRoot=std::filesystem::path(__FILE__).parent_path().parent_path()/"assets";
struct Mixer {
  ma_engine engine{};
  Mixer() {
    auto config=ma_engine_config_init();
    config.noDevice=MA_TRUE;config.channels=2;config.sampleRate=48000;
    REQUIRE(ma_engine_init(&config,&engine)==MA_SUCCESS);
  }
  ~Mixer() {ma_engine_uninit(&engine);}
  std::vector<float> read(size_t frames) {
    std::vector<float> pcm(frames*2);
    REQUIRE(ma_engine_read_pcm_frames(&engine,pcm.data(),frames,nullptr)==MA_SUCCESS);
    return pcm;
  }
};
double energy(const std::vector<float> &pcm) {
  double total=0;
  CHECK(std::all_of(pcm.begin(),pcm.end(),[](float sample){return std::isfinite(sample);}));
  for(float sample:pcm) total+=sample*sample;
  return total;
}
}

TEST_CASE("Audio default footsteps exclude metadata ambience and weapon clips") {
  const auto clips=audio::footstepClips(assetRoot,{},assetRoot/"cricket.ogg");
  REQUIRE(clips.size()==6);
  for(size_t i=0;i<clips.size();++i) {
    const auto name=std::filesystem::path(clips[i]).filename().string();
    CHECK(name.find("Fantozzi-Sand")==0);
    CHECK(name[13]==(i%2==0?'L':'R'));
  }
  CHECK_FALSE(audio::isAudioClip("._Fantozzi-SandL1.ogg"));
  CHECK_FALSE(audio::isAudioClip("hidden.txt"));
  CHECK(audio::footstepClips(assetRoot,assetRoot/"does-not-exist.wav",{}).empty());
  CHECK(audio::footstepClips(assetRoot,assetRoot/"cricket.ogg",assetRoot/"cricket.ogg").empty());
}

TEST_CASE("Footstep cadence follows travelled distance across frame rates") {
  for(int fps:{10,15,30,60,144}) {
    for(bool running:{false,true}) {
      audio::FootstepClock clock;
      const float speed=running?10.f:5.f,cadence=running?.24f:.34f;
      int contacts=0;
      for(int i=0;i<fps*10;++i)
        contacts+=clock.tick(1.f/fps,speed/fps,true,speed,cadence);
      CHECK(contacts==int(10.f/cadence+.5f));
    }
  }
}

TEST_CASE("Idle blocked airborne paused flight and disabled motion produce no footsteps") {
  audio::FootstepClock clock;
  for(int i=0;i<100;++i) {
    CHECK_FALSE(clock.tick(.1f,0,true,5,.34f));
    CHECK_FALSE(clock.tick(.1f,.5f,false,5,.34f));
    // Repeated tiny key taps cannot retrigger an immediate footfall.
    CHECK_FALSE(clock.tick(.01f,.05f,true,5,.34f));
    CHECK_FALSE(clock.tick(.1f,0,true,5,.34f));
  }
  CHECK_FALSE(clock.tick(.1f,100,true,5,.34f)); // Teleport.
  CHECK_FALSE(clock.tick(0,.5f,true,5,.34f));
  CHECK_FALSE(clock.tick(-1,.5f,true,5,.34f));
  CHECK_FALSE(clock.tick(std::numeric_limits<float>::quiet_NaN(),.5f,true,5,.34f));
  CHECK_FALSE(clock.tick(.1f,std::numeric_limits<float>::infinity(),true,5,.34f));
  CHECK_FALSE(clock.tick(.1f,.5f,true,5,.34f));
  CHECK(clock.tick(.1f,.5f,true,5,.34f));
  // A presentation hitch plays at most one contact, then resumes normal cadence.
  CHECK(clock.tick(1.f,5.f,true,5,.34f));
  CHECK_FALSE(clock.tick(.01f,.05f,true,5,.34f));
}

TEST_CASE("Real Vorbis footsteps decode once and replay complete audible PCM") {
  Mixer mixer;
  for(const auto &path:audio::footstepClips(assetRoot,{},assetRoot/"cricket.ogg")) {
    audio::Voice voice;
    REQUIRE(voice.load(mixer.engine,path,false,false)==MA_SUCCESS);
    CHECK(voice.bufferLoaded);CHECK_FALSE(voice.decoderLoaded);
    CHECK(voice.encodedData.empty());
    REQUIRE(voice.restart()==MA_SUCCESS);
    const auto first=mixer.read(48000);
    CHECK(energy(first)>1.e-4);
    CHECK_FALSE(ma_sound_is_playing(&voice.sound));
    REQUIRE(voice.restart()==MA_SUCCESS);
    const auto second=mixer.read(48000);
    // The graph retains a partial mixing block between reads, so the onset can
    // shift within that block. A complete second clip must retain its energy.
    CHECK(energy(second)==doctest::Approx(energy(first)).epsilon(.001));
  }
}

TEST_CASE("Ambient Vorbis loop remains audible beyond its end and gunfire still decodes") {
  Mixer mixer;
  audio::Voice ambient,shot;
  REQUIRE(ambient.load(mixer.engine,(assetRoot/"cricket.ogg").string(),true,true)==MA_SUCCESS);
  CHECK(ambient.decoderLoaded);CHECK_FALSE(ambient.bufferLoaded);
  REQUIRE(ma_sound_start(&ambient.sound)==MA_SUCCESS);
  ma_uint64 frames=0;
  REQUIRE(ma_sound_get_length_in_pcm_frames(&ambient.sound,&frames)==MA_SUCCESS);
  REQUIRE(frames>0);
  double total=0;
  for(ma_uint64 consumed=0;consumed<frames+48000;consumed+=48000)
    total+=energy(mixer.read(48000));
  CHECK(total>1.e-4);CHECK(ma_sound_is_playing(&ambient.sound));
  ma_sound_stop(&ambient.sound);
  REQUIRE(shot.load(mixer.engine,(assetRoot/"weapons/ak47/audio/shot0.wav").string(),false,false)==MA_SUCCESS);
  REQUIRE(shot.restart(.55f)==MA_SUCCESS);
  CHECK(energy(mixer.read(48000))>1.e-4);
}

TEST_CASE("Audio master volume is applied once and mute silences the real mix") {
  Mixer mixer;
  ma_sound_group group{};
  REQUIRE(ma_sound_group_init(&mixer.engine,MA_SOUND_FLAG_NO_SPATIALIZATION,nullptr,&group)==MA_SUCCESS);
  {
    audio::Voice step;
    REQUIRE(step.load(mixer.engine,(assetRoot/"Fantozzi-SandL1.ogg").string(),false,false,&group)==MA_SUCCESS);
    AudioSettings settings;settings.footstepVolume=.6f;
    audio::applyVolumes(mixer.engine,nullptr,&group,settings);
    REQUIRE(step.restart()==MA_SUCCESS);
    const auto full=mixer.read(48000);
    settings.masterVolume=.5f;
    audio::applyVolumes(mixer.engine,nullptr,&group,settings);
    REQUIRE(step.restart()==MA_SUCCESS);
    const auto half=mixer.read(48000);
    CHECK(energy(half)==doctest::Approx(energy(full)*.25).epsilon(.01));
    settings.mute=true;
    audio::applyVolumes(mixer.engine,nullptr,&group,settings);
    REQUIRE(step.restart()==MA_SUCCESS);
    CHECK(energy(mixer.read(48000))==0);
    settings.mute=false;settings.footstepsEnabled=false;
    audio::applyVolumes(mixer.engine,nullptr,&group,settings);
    REQUIRE(step.restart()==MA_SUCCESS);
    CHECK(energy(mixer.read(48000))==0);
  }
  ma_sound_group_uninit(&group);
}
