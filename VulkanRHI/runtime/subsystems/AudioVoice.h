#pragma once

#include "miniaudio.h"
#include "AudioSettings.h"
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace audio {
// Short effects are decoded before playback. The mixer only reads PCM; opening
// and decoding a new Vorbis stream on every step caused walking-time glitches.
// Long ambience retains a single decoder rather than allocating its whole loop.
struct Voice {
  ma_sound sound{};
  ma_decoder decoder{};
  ma_audio_buffer buffer{};
  void *pcm=nullptr;
  bool loaded=false,decoderLoaded=false,bufferLoaded=false;
  std::vector<ma_uint8> encodedData;
  Voice()=default;
  Voice(const Voice&)=delete;
  Voice& operator=(const Voice&)=delete;
  ~Voice() {unload();}

  void unload() {
    if(loaded) {ma_sound_uninit(&sound);loaded=false;}
    if(decoderLoaded) {ma_decoder_uninit(&decoder);decoderLoaded=false;}
    if(bufferLoaded) {ma_audio_buffer_uninit(&buffer);bufferLoaded=false;}
    ma_free(pcm,nullptr);pcm=nullptr;
    encodedData.clear();
  }
  ma_result load(ma_engine &engine,const std::string &path,bool looped,bool streamed,
                 ma_sound_group *group=nullptr) {
    unload();
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) return MA_DOES_NOT_EXIST;
    const auto size=file.tellg();
    if(size<=0) return MA_INVALID_FILE;
    encodedData.resize(static_cast<size_t>(size));
    file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(encodedData.data()),size)) return MA_IO_ERROR;
    auto config=ma_decoder_config_init(ma_format_f32,ma_engine_get_channels(&engine),
                                      ma_engine_get_sample_rate(&engine));
    ma_result result;
    ma_data_source *source;
    if(streamed) {
      result=ma_decoder_init_memory(encodedData.data(),encodedData.size(),&config,&decoder);
      if(result!=MA_SUCCESS) {unload();return result;}
      decoderLoaded=true;source=&decoder;
    } else {
      ma_uint64 frames=0;
      result=ma_decode_memory(encodedData.data(),encodedData.size(),&config,&frames,&pcm);
      if(result!=MA_SUCCESS || !frames) {unload();return result==MA_SUCCESS?MA_INVALID_FILE:result;}
      auto bufferConfig=ma_audio_buffer_config_init(ma_format_f32,config.channels,frames,pcm,nullptr);
      bufferConfig.sampleRate=config.sampleRate;
      result=ma_audio_buffer_init(&bufferConfig,&buffer);
      if(result!=MA_SUCCESS) {unload();return result;}
      bufferLoaded=true;source=&buffer;
      encodedData.clear();
    }
    result=ma_sound_init_from_data_source(&engine,source,MA_SOUND_FLAG_NO_SPATIALIZATION,group,&sound);
    if(result!=MA_SUCCESS) {unload();return result;}
    loaded=true;
    ma_sound_set_looping(&sound,looped?MA_TRUE:MA_FALSE);
    return MA_SUCCESS;
  }
  ma_result restart(float volume=1) {
    if(!loaded) return MA_INVALID_OPERATION;
    // Miniaudio queues the seek on the mixer thread; never seek the decoder directly.
    ma_sound_stop(&sound);
    auto result=ma_sound_seek_to_pcm_frame(&sound,0);
    if(result!=MA_SUCCESS) return result;
    ma_sound_set_volume(&sound,volume);
    return ma_sound_start(&sound);
  }
};

inline void applyVolumes(ma_engine &engine,Voice *ambient,ma_sound_group *footsteps,
                         const AudioSettings &settings) {
  // Master belongs only on the engine. Multiplying it into categories too
  // squared the volume slider and made ambience/steps behave unlike gunfire.
  ma_engine_set_volume(&engine,settings.enabled && !settings.mute?
      std::clamp(settings.masterVolume,0.f,1.5f):0.f);
  if(ambient && ambient->loaded)
    ma_sound_set_volume(&ambient->sound,std::clamp(settings.ambientVolume,0.f,1.5f));
  if(footsteps)
    ma_sound_group_set_volume(footsteps,settings.footstepsEnabled?
        std::clamp(settings.footstepVolume,0.f,1.5f):0.f);
}
} // namespace audio
