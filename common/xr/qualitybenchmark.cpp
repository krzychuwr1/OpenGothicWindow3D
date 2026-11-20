#ifdef OPENGOTHIC_XR
#include "qualitybenchmark.h"

#include <Tempest/Application>
#include <Tempest/Log>

#include "gothic.h"
#include "utils/inifile.h"
#include "xrwindow.h"

namespace {
IniFile* overrides = nullptr;
std::string sequence;
size_t phase = 0;
uint64_t next = 0;
bool measuring = false;
}

void XrQualityBenchmark::configure(IniFile& ini) {
  overrides = nullptr;
  phase = 0;
  next = 0;
  measuring = false;
  sequence = ini.getS("BENCHMARK","xrQualitySweep");
  if(sequence.empty())
    return;
  if(sequence.find_first_not_of("123456")!=std::string::npos) {
    Tempest::Log::e("Invalid xrQualitySweep phase list: ",sequence);
    return;
    }
  overrides = &ini;
  }

void XrQualityBenchmark::tick() {
  if(!overrides)
    return;
  const uint64_t now = Tempest::Application::tickCount();
  if(now<next)
    return;
  if(measuring) {
    Tempest::Log::i("XR quality ",int(sequence[phase]-'0')," end");
    measuring = false;
    if(++phase==sequence.size()) {
      overrides = nullptr;
      return;
      }
    } else if(next!=0) {
    Tempest::Log::i("XR quality ",int(sequence[phase]-'0')," begin");
    measuring = true;
    next = now+12000;
    return;
    }
  // 75%, native, 125%, 150%, 75% + CMAA2, 75% + compositor filtering.
  static constexpr int resolutions[] = {1,0,3,4,1,1};
  const int id = sequence[phase]-'0';
  overrides->set("INTERNAL","vidResIndex",resolutions[id-1]);
  overrides->set("INTERNAL","xrCmaa2",int(id==5));
  overrides->set("INTERNAL","xrWindowFilter",int(id==6));
  Gothic::inst().onSettingsChanged();
  const auto* xr = XrWindow::inst();
  Tempest::Log::i("XR quality ",id," warmup resIndex=",resolutions[id-1]," aa=",int(id==5),
                  " filter=",int(id==6 && xr && xr->isWindowFilterSupported()));
  next = Tempest::Application::tickCount()+5000;
  }
#endif
