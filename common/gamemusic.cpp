#include "gamemusic.h"
#include "gothic.h"

#include <Tempest/Sound>
#include <Tempest/Log>

#include "game/definitions/musicdefinitions.h"
#include "dmusic/mixer.h"
#include "resources.h"
#include "dmusic.h"

using namespace Tempest;

static constexpr uint16_t SAMPLE_RATE = 44100;

struct GameMusic::MusicProvider : Tempest::SoundProducer {
  using Tempest::SoundProducer::SoundProducer;

  public:
    void setEnabled(bool b) {
      std::lock_guard<std::mutex> guard(pendingSync);
      if(enable == b)
        return;

      if(b) {
        hasPending = true;
        reloadTheme = true;
        enable.store(true);
        } else {
        enable.store(false);
        pendingMusic.reset();
        stopPending = true;
        }
      }

    bool isEnabled() const {
      return enable.load();
      }

    void playTheme(const zenkit::IMusicTheme &theme, GameMusic::Tags tags) {
      std::lock_guard<std::mutex> guard(pendingSync);
      reloadTheme |= !pendingMusic || pendingMusic->file != theme.file;
      pendingMusic = theme;
      pendingTags  = tags;
      hasPending   = true;
      }

  protected:
    enum class Request { None, Play, Stop };
    enum class OnStop  { Mute, FlushStream };

    Request takeRequest(zenkit::IMusicTheme& theme, Tags& tags, OnStop onStop) {
      std::lock_guard<std::mutex> guard(pendingSync);
      if(stopPending) {
        stopPending = false;
        if(onStop==OnStop::FlushStream)
          return Request::Stop;
        }
      if(!hasPending || !pendingMusic || !enable.load())
        return Request::None;
      hasPending = false;
      if(!reloadTheme)
        return Request::None;
      theme = *pendingMusic;
      tags  = pendingTags;
      reloadTheme = false;
      return Request::Play;
      }

    bool takeStopDuringLoad() {
      std::lock_guard<std::mutex> guard(pendingSync);
      bool stop = stopPending;
      stopPending = false;
      return stop;
      }

    void retryThemeOnRequest() {
      std::lock_guard<std::mutex> guard(pendingSync);
      // Another zone may already have queued a replacement while this theme loaded.
      reloadTheme = true;
      }

    static zenkit::MusicTransitionEffect computeTransitionEffect(Tags nextTags, Tags currTags, zenkit::MusicTransitionEffect transtype) {
      const int cur  = currTags & (Tags::Std | Tags::Fgt | Tags::Thr);
      const int next = nextTags & (Tags::Std | Tags::Fgt | Tags::Thr);

      //zenkit::MusicTransitionEffect embellishment = transtype;
      zenkit::MusicTransitionEffect embellishment = zenkit::MusicTransitionEffect::NONE;
      if(next == Tags::Std) {
        if(cur != Tags::Std)
          embellishment = zenkit::MusicTransitionEffect::BREAK;
        }
      else if(next == Tags::Fgt) {
        if(cur == Tags::Thr)
          embellishment = zenkit::MusicTransitionEffect::FILL;
        }
      else if(next == Tags::Thr) {
        if(cur == Tags::Fgt)
          embellishment = zenkit::MusicTransitionEffect::NONE;
        }

      return embellishment;
      }

  private:
    std::atomic_bool     enable{true};

    std::mutex           pendingSync;
    bool                 stopPending = false;
    bool                 hasPending  = false;
    bool                 reloadTheme = false;
    Tags                 pendingTags = Tags::Day;
    std::optional<zenkit::IMusicTheme> pendingMusic;

  protected:
    Tags                 currentTags = Tags::Day;
  };

struct GameMusic::OpenGothicMusicProvider : GameMusic::MusicProvider {
  using GameMusic::MusicProvider::MusicProvider;

  void renderSound(int16_t *out, size_t n) override {
    if(!isEnabled()) {
      std::memset(out, 0, n * sizeof(out[0]) * 2);
      return;
      }
    updateTheme();
    mix.mix(out, n);
    }

  void updateTheme() {
    zenkit::IMusicTheme theme;
    Tags                tags;
    if(takeRequest(theme,tags,OnStop::Mute)!=Request::Play)
      return;

    if(theme.file.empty()) {
      retryThemeOnRequest();
      return;
      }

    try {
      if(/*reloadTheme*/true) {
        Dx8::PatternList p = Resources::loadDxMusic(theme.file);

        Dx8::Music m;
        m.addPattern(p);

        auto effect = computeTransitionEffect(tags, currentTags, theme.transtype);
        Dx8::DMUS_EMBELLISHT_TYPES em = computeEmbellishment(effect);
        mix.setMusic(m, em);
        currentTags = tags;
        }
      mix.setMusicVolume(theme.vol);
      }
    catch (std::runtime_error &) {
      Log::e("unable to load sound: \"", theme.file, "\"");
      retryThemeOnRequest();
      }
    catch (std::bad_alloc &) {
      Log::e("out of memory for sound: \"", theme.file, "\"");
      retryThemeOnRequest();
      }
    }

  static Dx8::DMUS_EMBELLISHT_TYPES computeEmbellishment(zenkit::MusicTransitionEffect ef) {
    switch (ef) {
      case zenkit::MusicTransitionEffect::UNKNOWN:
      case zenkit::MusicTransitionEffect::NONE:
        return Dx8::DMUS_EMBELLISHT_NORMAL;
      case zenkit::MusicTransitionEffect::GROOVE:
        return Dx8::DMUS_EMBELLISHT_NORMAL;
      case zenkit::MusicTransitionEffect::FILL:
        return Dx8::DMUS_EMBELLISHT_FILL;
      case zenkit::MusicTransitionEffect::BREAK:
        return Dx8::DMUS_EMBELLISHT_BREAK;
      case zenkit::MusicTransitionEffect::INTRO:
        return Dx8::DMUS_EMBELLISHT_INTRO;
      case zenkit::MusicTransitionEffect::END:
      case zenkit::MusicTransitionEffect::END_AND_INTO:
        return Dx8::DMUS_EMBELLISHT_END;
      }
    return Dx8::DMUS_EMBELLISHT_NORMAL;
    }

  private:
    Dx8::Mixer mix;
  };

static std::pair<DmTiming, DmEmbellishmentType> computeEmbellishmentAndTiming(const zenkit::MusicTransitionEffect effect, const zenkit::MusicTransitionType type) {
  DmEmbellishmentType embellishment = DmEmbellishment_NONE;
  switch (effect) {
    case zenkit::MusicTransitionEffect::UNKNOWN:
    case zenkit::MusicTransitionEffect::NONE:
      embellishment = DmEmbellishment_NONE;
      break;
    case zenkit::MusicTransitionEffect::GROOVE:
      embellishment = DmEmbellishment_GROOVE;
      break;
    case zenkit::MusicTransitionEffect::FILL:
      embellishment = DmEmbellishment_FILL;
      break;
    case zenkit::MusicTransitionEffect::BREAK:
      embellishment = DmEmbellishment_BREAK;
      break;
    case zenkit::MusicTransitionEffect::INTRO:
      embellishment = DmEmbellishment_INTRO;
      break;
    case zenkit::MusicTransitionEffect::END:
      embellishment = DmEmbellishment_END;
      break;
    case zenkit::MusicTransitionEffect::END_AND_INTO:
      embellishment = DmEmbellishment_END_AND_INTRO;
      break;
    }

  DmTiming timing = DmTiming_MEASURE;
  switch (type) {
    case zenkit::MusicTransitionType::UNKNOWN:
    case zenkit::MusicTransitionType::MEASURE:
      timing = DmTiming_MEASURE;
      break;
    case zenkit::MusicTransitionType::IMMEDIATE:
      timing = DmTiming_INSTANT;
      break;
    case zenkit::MusicTransitionType::BEAT:
      timing = DmTiming_BEAT;
      break;
    }

  return std::make_pair(timing, embellishment);
  }

struct GameMusic::GothicKitMusicProvider : GameMusic::MusicProvider {
  GothicKitMusicProvider(uint16_t rate, uint16_t channels) : GameMusic::MusicProvider(rate, channels) {
    /*
    Dm_setRandomNumberGenerator([](void*) -> uint32_t {
      return 0;
      }, nullptr);
    */

    DmResult rv = DmPerformance_create(&performance, rate);
    if(rv != DmResult_SUCCESS) {
      Log::e("Unable to create DmPerformance object. Out of memory?");
      }
    }

  ~GothicKitMusicProvider() override {
    DmPerformance_release(performance);
    }

  void renderSound(int16_t* out, size_t n) override {
    const auto format = DmRenderOptions(DmRender_SHORT | DmRender_STEREO);
    if(performance!=nullptr) {
      zenkit::IMusicTheme theme;
      Tags tags;
      auto request = retryStop ? Request::Stop : takeRequest(theme,tags,OnStop::FlushStream);
      if(request==Request::Play) {
        bool played = startTheme(theme,tags);
        if(!played)
          retryThemeOnRequest();
        if(takeStopDuringLoad() || !played)
          request = Request::Stop;
        }

      if(request==Request::Stop) {
        const auto stop = DmPerformance_playSegment(performance, nullptr, DmTiming_INSTANT);
        const auto pcm  = DmPerformance_renderPcm(performance, out, n * 2, format);
        retryStop = stop!=DmResult_SUCCESS || pcm!=DmResult_SUCCESS;
        }
      else if(isEnabled() && DmPerformance_renderPcm(performance, out, n * 2, format)==DmResult_SUCCESS) {
        return;
        }
      }
    std::memset(out, 0, n * sizeof(out[0]) * 2);
    }

  bool startTheme(const zenkit::IMusicTheme& theme, Tags tags) {
    if(theme.file.empty())
      return false;

    auto effect = computeTransitionEffect(tags, currentTags, theme.transtype);
    auto [timing, embellishment] = computeEmbellishmentAndTiming(effect, theme.transsubtype);

    DmSegment* sgt = Resources::loadMusicSegment(theme.file.c_str());
    if(sgt==nullptr)
      return false;
    DmResult rv = DmPerformance_playTransition(performance, sgt, embellishment, timing);
    DmSegment_release(sgt);
    if(rv != DmResult_SUCCESS) {
      Log::e("Failed to play theme: ", theme.file);
      return false;
      }

    DmPerformance_setVolume(performance, theme.vol);
    currentTags = tags;
    return true;
    }

  DmEmbellishmentType computeEmbellishment(Tags nextTags, Tags currTags, zenkit::MusicTransitionEffect transtype) const {
    const int cur  = currTags & (Tags::Std | Tags::Fgt | Tags::Thr);
    const int next = nextTags & (Tags::Std | Tags::Fgt | Tags::Thr);

    DmEmbellishmentType embellishment = DmEmbellishment_NONE;
    switch (transtype) {
      case zenkit::MusicTransitionEffect::UNKNOWN:
      case zenkit::MusicTransitionEffect::NONE:
        embellishment = DmEmbellishment_NONE;
        break;
      case zenkit::MusicTransitionEffect::GROOVE:
        embellishment = DmEmbellishment_GROOVE;
        break;
      case zenkit::MusicTransitionEffect::FILL:
        embellishment = DmEmbellishment_FILL;
        break;
      case zenkit::MusicTransitionEffect::BREAK:
        embellishment = DmEmbellishment_BREAK;
        break;
      case zenkit::MusicTransitionEffect::INTRO:
        embellishment = DmEmbellishment_INTRO;
        break;
      case zenkit::MusicTransitionEffect::END:
        embellishment = DmEmbellishment_END;
        break;
      case zenkit::MusicTransitionEffect::END_AND_INTO:
        embellishment = DmEmbellishment_END_AND_INTRO;
        break;
      }

    if(next == Tags::Std) {
      if(cur != Tags::Std)
        embellishment = DmEmbellishmentType::DmEmbellishment_BREAK;
      }
    else if(next == Tags::Fgt) {
      if(cur == Tags::Thr)
        embellishment = DmEmbellishmentType::DmEmbellishment_FILL;
      }
    else if(next == Tags::Thr) {
      if(cur == Tags::Fgt)
        embellishment = DmEmbellishmentType::DmEmbellishment_NONE;
      }

    return embellishment;
    }

  private:
    DmPerformance*    performance = nullptr;
    bool              retryStop = false;
  };

static constexpr int PROVIDER_UNINITIALIZED = -1;
static constexpr int PROVIDER_OPENGOTHIC = 0;
// static constexpr int PROVIDER_GOTHICKIT = 1;

GameMusic *GameMusic::instance = nullptr;

GameMusic::GameMusic() {
  instance = this;
  provider = PROVIDER_UNINITIALIZED;

  Gothic::inst().onSettingsChanged.bind(this, &GameMusic::setupSettings);
  setupSettings();
  }

GameMusic::~GameMusic() {
  sound = SoundEffect();
  instance = nullptr;
  Gothic::inst().onSettingsChanged.ubind(this, &GameMusic::setupSettings);
  }

GameMusic &GameMusic::inst() {
  return *instance;
  }

GameMusic::Tags GameMusic::mkTags(GameMusic::Tags daytime, GameMusic::Tags mode) {
  return Tags(daytime | mode);
  }

void GameMusic::setEnabled(bool e) {
  impl->setEnabled(e);
  }

bool GameMusic::isEnabled() const {
  return impl->isEnabled();
  }

void GameMusic::setMusic(GameMusic::Music m) {
  const char *clsTheme = "";
  switch (m) {
    case GameMusic::SysLoading:
      clsTheme = "SYS_Loading";
      break;
    }
  if(auto theme = Gothic::musicDef()[clsTheme])
    setMusic(*theme, GameMusic::mkTags(GameMusic::Std, GameMusic::Day));
  }

void GameMusic::setMusic(const zenkit::IMusicTheme &theme, Tags tags) {
  currentMusic.theme = theme;
  currentMusic.tags  = tags;
  impl->playTheme(currentMusic.theme, currentMusic.tags);
  }

void GameMusic::setVideoActive(bool active) {
  if(active==videoActive)
    return;
  videoActive = active;
  if(active)
    sound.pause();
  else
    sound.play();
  }

void GameMusic::stopMusic() {
  setEnabled(false);
  }

void GameMusic::setupSettings() {
  const bool  musicEnabled  = Gothic::settingsGetI("SOUND", "musicEnabled")!=0;
  const float musicVolume   = Gothic::settingsGetF("SOUND", "musicVolume");
  const int   providerIndex = Gothic::settingsGetI("INTERNAL", "soundProviderIndex");

  if(impl!=nullptr && providerIndex==provider && musicEnabled==isEnabled()) {
    sound.setVolume(musicVolume);
    return;
    }

  Log::i("Switching music provider to ", providerIndex == PROVIDER_OPENGOTHIC ? "'OpenGothic'" : "'GothicKit'");
  std::unique_ptr<MusicProvider> p;
  if(providerIndex == PROVIDER_OPENGOTHIC)
    p = std::make_unique<OpenGothicMusicProvider>(SAMPLE_RATE, 2);
  else
    p = std::make_unique<GothicKitMusicProvider>(SAMPLE_RATE, 2);

  p->playTheme(currentMusic.theme, currentMusic.tags);
  p->setEnabled(musicEnabled);
  auto* next = p.get();
  sound = device.load(std::move(p));
  impl = next;
  provider = providerIndex;
  sound.setVolume(musicVolume);
  if(!videoActive)
    sound.play();
  }
