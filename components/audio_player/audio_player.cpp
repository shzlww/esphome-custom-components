#include "audio_player.h"
#include "esphome/core/log.h"
#include <cstring>

#include "AudioGeneratorWAV.h"
#include "AudioGeneratorAAC.h"
#include "AudioGeneratorMP3.h"
#include "AudioFileSourceHTTPStream.h"
#include "CustomAudioFileSourceHTTPStream.h"

namespace esphome {
namespace audio_player {

static const char *const TAG = "audio_player";

// Called when there's a warning or error (like a buffer underflow or decode hiccup)
void StatusCallback(void *cbData, int code, const char *string) {
  const char *ptr = reinterpret_cast<const char *>(cbData);
  // Note that the string may be in PROGMEM, so copy it to RAM for printf
  char s1[64];
  strncpy_P(s1, string, sizeof(s1));
  s1[sizeof(s1) - 1] = 0;
  ESP_LOGD(TAG, "STATUS(%s) '%d' = '%s'", ptr, code, s1);
}

void AudioMediaPlayer::setup() {
  if (out_ == nullptr) {
    this->mark_failed();
    return;
  }

  this->volume_contorller_->set_volume(base_volume_);
  audioLogger = &Serial;
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
}

void AudioMediaPlayer::dump_config() {
  ESP_LOGCONFIG(TAG, "Audio Player:");
  ESP_LOGCONFIG(TAG, "  Buffer Size: %d", this->buffer_size_);
  ESP_LOGCONFIG(TAG, "  Base Volume: %d%%", (int) (this->base_volume_ * 100));
  ESP_LOGCONFIG(TAG, "  Current Volume: %d%%", (int) this->volume * 100);
  ESP_LOGCONFIG(TAG, "  Muted: %s", this->muted_ ? "yes" : "no");
  if (this->ext_info_ != nullptr) {
    this->ext_info_->dump_config();
  }
}

void AudioMediaPlayer::loop() {
  if (generator_ == nullptr) {
    return;
  }
  if (!generator_->isRunning()) {
    if(this->state != media_player::MEDIA_PLAYER_STATE_IDLE) {
       ESP_LOGD(TAG, "audio done,stop it 0!");
       this->stop();
       this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
       this->publish_state();
       return;
    }
    return;
  }
  if (!this->pause_ && generator_->loop()) {
    return;
  }
  //处于运行状态但是播放循环已结束，停止播放并发布状态
  if (generator_->isRunning()) {
    if(!generator_->loop()) {
       ESP_LOGD(TAG, "audio done,stop it!");
       this->stop();
       this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
       this->publish_state();
       return;
    }
  }
}

void AudioMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (call.get_media_url().has_value()) {
    this->high_freq_.start();
    if(!this->play(call.get_media_url().value())) {
      this->high_freq_.stop();
    }
  }
  if (call.get_volume().has_value()) {
    ESP_LOGD(TAG, "volume set %.2f", call.get_volume().value());
    this->set_volume(call.get_volume().value());
    this->unmute_();
  }
  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
/*       case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        this->pause_ = false;
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        break;
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        this->pause_ = true;
        this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        break;
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (this->pause_) {
          this->pause_ = false;
          this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
        } else {
          this->pause_ = true;
          this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        }
        break; */
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        this->stop();
        this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
        break;
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->mute_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->unmute_();
        break;
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        float new_volume = this->volume + 0.1f;
        if (new_volume > 1.0f)
          new_volume = 1.0f;
        this->set_volume(new_volume);
        this->unmute_();
        break;
      }
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        float new_volume = this->volume - 0.1f;
        if (new_volume < 0.0f)
          new_volume = 0.0f;
        this->set_volume(new_volume);
        this->unmute_();
        break;
      }
    }
  }
  this->publish_state();
}

media_player::MediaPlayerTraits AudioMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(false);
  return traits;
};

void AudioMediaPlayer::stop() {
  if (generator_ != NULL) {
    if (generator_->isRunning()) {
      generator_->stop();
      this->high_freq_.stop();
    }
    delete generator_;
    generator_ = NULL;
  }

  if (buffer_ != NULL) {
    delete buffer_;
    buffer_ = NULL;
  }
  if (file_ != NULL) {
    delete file_;
    file_ = NULL;
  }
}


bool AudioMediaPlayer::play(const std::string &url) {
  stop();

  ESP_LOGD(TAG, "play url %s", url.c_str());
  if (url.rfind("http://", 0) != 0) {
    ESP_LOGE(TAG, "Unsupported protocol");
    return false;
  }

  CustomAudioFileSourceHTTPStream *http_stream = new CustomAudioFileSourceHTTPStream(url.c_str());
  
  std::string content_type = http_stream->contentType();
  ESP_LOGD(TAG, "url content_type is[%s]", content_type.c_str());
  if (content_type.empty()) {
    ESP_LOGE(TAG, "http: no Content-Type");
    generator_ = new AudioGeneratorWAV();
  } else if (content_type == "audio/wav" || content_type == "audio/x-wav") {
    generator_ = new AudioGeneratorWAV();
  } else if (content_type == "audio/mp3" || content_type == "audio/mpeg") {
    generator_ = new AudioGeneratorMP3();
  } else if (content_type == "audio/aac") {
    generator_ = new AudioGeneratorAAC();
  } else {
    ESP_LOGW(TAG, "http: Unsupported Content-type:%s", content_type.c_str());
    generator_ = new AudioGeneratorWAV();
  }

  this->file_ = http_stream;
  this->buffer_ = new AudioFileSourceBuffer(file_, buffer_size_);
  //this->generator_ = new AudioGeneratorWAV();

  file_->RegisterStatusCB(StatusCallback, (void *) "file");
  buffer_->RegisterStatusCB(StatusCallback, (void *) "buffer");
  generator_->RegisterStatusCB(StatusCallback, (void *) "decoder");

  if (!generator_->begin(buffer_, out_)) {
    ESP_LOGE(TAG, "play failed");
    stop();
    this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    this->publish_state();
    return false;
  }

  this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
  this->publish_state();
  return true;
}

void AudioMediaPlayer::mute_() {
  this->muted_ = true;
  volume_contorller_->set_volume(0);
}

void AudioMediaPlayer::unmute_() {
  this->muted_ = false;
  volume_contorller_->set_volume(this->volume * base_volume_);
}


void AudioMediaPlayer::set_volume(float volume) {
  this->volume = volume;
  if (this->muted_) {
    volume_contorller_->set_volume(0);
  } else {
    volume_contorller_->set_volume(this->volume * base_volume_);
  }
}


void PlayerOutputI2S::set_pins(InternalGPIOPin *bclk, InternalGPIOPin *wclk, InternalGPIOPin *dout) {
  this->bclk_ = bclk;
  this->wclk_ = wclk;
  this->dout_ = dout;
  this->SetPinout(bclk->get_pin(), wclk->get_pin(), dout->get_pin());
}

void PlayerOutputI2S::dump_config() {
  ESP_LOGCONFIG(TAG, "  OutPut: i2s");
  LOG_PIN("    BCLK Pin: ", this->bclk_);
  LOG_PIN("    WCLK Pin: ", this->wclk_);
  LOG_PIN("    DOUT Pin: ", this->dout_);
}

void PlayerOutputI2SNoDAC::dump_config() { ESP_LOGCONFIG(TAG, "    Output: i2sNoDAC"); }


}  // namespace audio_player
}  // namespace esphome
