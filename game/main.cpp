#include <Tempest/Window>
#include <Tempest/Application>
#include <Tempest/Log>

#include <zenkit/Logger.hh>

#include <Tempest/VulkanApi>
#if !defined(__APPLE__)
#include <Tempest/VulkanInterop>
#endif

#if defined(_MSC_VER)
#include <Tempest/DirectX12Api>
#endif

#if defined(__APPLE__)
#include <Tempest/MetalApi>
#endif

#if defined(__IOS__) || defined(__ANDROID__)
#include "utils/installdetect.h"
#include <filesystem>
#endif

#include "utils/crashlog.h"
#include "mainwindow.h"
#include "gothic.h"
#include "build.h"
#include "commandline.h"
#include "xr/xrwindow.h"

#include <dmusic.h>

#include <cctype>
#include <fstream>
#include <string>
#include <vector>

std::string_view selectDevice(const Tempest::AbstractGraphicsApi& api) {
  auto d = api.devices();

  static Tempest::Device::Props p;
  for(auto& i:d)
    // if(i.type==Tempest::DeviceType::Integrated) {
    if(i.type==Tempest::DeviceType::Discrete) {
      p = i;
      return p.name;
      }
  if(d.size()>0) {
    p = d[0];
    return p.name;
    }
  return "";
  }

std::unique_ptr<Tempest::AbstractGraphicsApi> mkApi(const CommandLine& g) {
  Tempest::ApiFlags flg = g.isValidationMode() ? Tempest::ApiFlags::Validation : Tempest::ApiFlags::NoFlags;
  switch(g.graphicsApi()) {
    case CommandLine::DirectX12:
#if defined(_MSC_VER)
      return std::make_unique<Tempest::DirectX12Api>(flg);
#else
      break;
#endif
    case CommandLine::Vulkan:
#if !defined(__APPLE__)
      return std::make_unique<Tempest::VulkanApi>(flg);
#else
      break;
#endif
    }

#if defined(__APPLE__)
  return std::make_unique<Tempest::MetalApi>(flg);
#else
  return std::make_unique<Tempest::VulkanApi>(flg);
#endif
  }

#if defined(__ANDROID__)
// Android starts the game without arguments: developer options can be listed in
// /sdcard/OpenGothic/args.txt (whitespace separated, "quoted" for spaces).
static std::vector<std::string> androidArgs() {
  std::vector<std::string> ret = {"app"};
  std::ifstream f("/storage/emulated/0/OpenGothic/args.txt");
  if(!f)
    return ret;
  std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  std::string cur;
  bool        quoted = false, has = false;
  for(char c:text) {
    if(c=='"') { quoted = !quoted; has = true; continue; }
    if(!quoted && std::isspace(static_cast<unsigned char>(c))) {
      if(has)
        ret.push_back(cur);
      cur.clear();
      has = false;
      continue;
      }
    cur.push_back(c);
    has = true;
    }
  if(has)
    ret.push_back(cur);
  return ret;
  }
#endif

int main(int argc,const char** argv) {
#if defined(__ANDROID__)
  static const std::vector<std::string> extraArgs = androidArgs();
  static std::vector<const char*>       extraArgv;
  for(auto& a:extraArgs)
    extraArgv.push_back(a.c_str());
  if(extraArgv.size()>1) {
    argc = int(extraArgv.size());
    argv = extraArgv.data();
    }
#endif
#if defined(__ANDROID__)
  {
    auto appdir = InstallDetect::androidInternalDataPath();
    std::filesystem::current_path(appdir);
  }
#elif defined(__IOS__)
  {
    auto appdir = InstallDetect::applicationSupportDirectory();
    std::filesystem::current_path(appdir);
  }
#endif

  try {
    static Tempest::WFile logFile("log.txt");
    Tempest::Log::setOutputCallback([](Tempest::Log::Mode mode, const char* text) {
      logFile.write(text,std::strlen(text));
      logFile.write("\n",1);
      if(mode==Tempest::Log::Error)
        logFile.flush();
      });
    }
  catch(...) {
    Tempest::Log::e("unable to setup logfile - fallback to console log");
    }
  CrashLog::setup();

  zenkit::Logger::set(zenkit::LogLevel::INFO, [] (zenkit::LogLevel lvl, const char* cat, const char* message) {
    (void)cat;
    switch (lvl) {
      case zenkit::LogLevel::ERROR:
        Tempest::Log::e("[zenkit] ", message);
        break;
      case zenkit::LogLevel::WARNING:
        Tempest::Log::e("[zenkit] ", message);
        break;
      case zenkit::LogLevel::INFO:
        Tempest::Log::i("[zenkit] ", message);
        break;
      case zenkit::LogLevel::DEBUG:
      case zenkit::LogLevel::TRACE:
        Tempest::Log::d("[zenkit] ", message); // unused
        break;
      }
    });
  Dm_setLogger(DmLogLevel_INFO, [](void* ctx, DmLogLevel lvl, char const* msg) {
    switch (lvl) {
      case DmLogLevel_FATAL:
      case DmLogLevel_ERROR:
      case DmLogLevel_WARN:
        Tempest::Log::e("[dmusic] ", msg);
        break;
      case DmLogLevel_INFO:
        Tempest::Log::i("[dmusic] ", msg);
        break;
      case DmLogLevel_DEBUG:
      case DmLogLevel_TRACE:
        Tempest::Log::d("[dmusic] ", msg);
        break;
      }
    }, nullptr);

  Tempest::Log::i(appBuild);
  Workers::setThreadName("Main thread");

  CommandLine          cmd{argc,argv};
#if !defined(__APPLE__)
  Tempest::vulkanSetGpuProfiling(cmd.isGpuProfile());
#endif
  // OpenXR decides which Vulkan instance and device the game uses, so it starts first
  XrWindow::preInit();
  auto                 api     = mkApi(cmd);
  const auto           gpuName = selectDevice(*api);
  CrashLog::setGpu(gpuName);

  Tempest::Device      device{*api,gpuName};
  CrashLog::setGpu(device.properties().name);
  if(auto xr = XrWindow::inst())
    xr->attach(device);

  Resources            resources{device};
  Gothic               gothic;
  GameMusic            music;
  gothic.setupGlobalScripts();

  MainWindow           wx(device);
  Tempest::Application app;
  return app.exec();
  }
