#include "xrwindow.h"
#include "commandline.h"

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)

#include "gothic.h"
#include "resources.h"
#include "shadingrate.h"
#include "envdepth.h"
#include "surfacelight.h"
#include "spatialpermission.h"
#include "shader.h"
#include "graphics/renderer.h"

#include <Tempest/VulkanInterop>
#include <Tempest/AndroidApi>
#include <Tempest/Log>
#include <Tempest/SystemApi>

#include <android/native_activity.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace Tempest;

namespace {

// Small pose math in metres, OpenXR conventions: +Y up, -Z forward.
struct V3 {
  float x=0, y=0, z=0;
  V3 operator+(const V3& o) const { return {x+o.x, y+o.y, z+o.z}; }
  V3 operator-(const V3& o) const { return {x-o.x, y-o.y, z-o.z}; }
  V3 operator*(float s)     const { return {x*s, y*s, z*s}; }
  };
static float dot(const V3& a, const V3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float length(const V3& a) { return std::sqrt(dot(a,a)); }
static V3    normalize(const V3& a) { const float l = length(a); return l>0 ? a*(1.f/l) : V3{0,0,-1}; }

static V3 toV3(const XrVector3f& v) { return {v.x, v.y, v.z}; }
static XrVector3f toXr(const V3& v) { return {v.x, v.y, v.z}; }

static V3 rotate(const XrQuaternionf& q, const V3& v) {
  const V3 u{q.x, q.y, q.z};
  const V3 t = V3{u.y*v.z - u.z*v.y, u.z*v.x - u.x*v.z, u.x*v.y - u.y*v.x}*2.f;
  const V3 c{u.y*t.z - u.z*t.y, u.z*t.x - u.x*t.z, u.x*t.y - u.y*t.x};
  return v + t*q.w + c;
  }

static XrQuaternionf conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

// Orientation whose +Z points along `back` (from the window towards the viewer), without roll.
static XrQuaternionf facing(const V3& back) {
  const V3    b     = normalize(back);
  const float yaw   = std::atan2(b.x, b.z);
  const float pitch = -std::asin(std::clamp(b.y, -1.f, 1.f));
  const float cy = std::cos(yaw*0.5f),   sy = std::sin(yaw*0.5f);
  const float cp = std::cos(pitch*0.5f), sp = std::sin(pitch*0.5f);
  // yaw around Y, then pitch around X
  return XrQuaternionf{cy*sp, sy*cp, -sy*sp, cy*cp};
  }

static bool ok(XrResult r, const char* what) {
  if(XR_SUCCEEDED(r))
    return true;
  Log::e("OpenXR: ", what, " failed (", int(r), ")");
  return false;
  }

static V3 toLocal(const XrPosef& pose, const V3& p) {
  return rotate(conjugate(pose.orientation), p - toV3(pose.position));
  }

static V3 toWorld(const XrPosef& pose, const V3& p) {
  return toV3(pose.position) + rotate(pose.orientation, p);
  }

// Parts of the window frame a hand can grab
enum class Grip : uint8_t { None, Handle, CornerLeft, CornerRight, Environment, Dim };

struct Hand {
  XrPath         path        = XR_NULL_PATH;
  XrSpace        aimSpace    = XR_NULL_HANDLE;   // pointing ray
  XrSpace        nearSpace   = XR_NULL_HANDLE;   // pinch point (hands) or grip (controllers), for direct touch
  bool           aimValid    = false;
  bool           nearValid   = false;
  XrPosef        aim         = {{0,0,0,1},{0,0,0}};
  XrPosef        nearPose    = {{0,0,0,1},{0,0,0}};
  std::optional<float> grab; // Only an active action can report a physical release.
  std::optional<float> trigger;
  bool           triggerHeld = true;
  bool           grabHeld    = true;
  Grip           hover       = Grip::None;
  bool           hoverNear   = false;
  V3             hit;                            // where the hand targets the frame part, local space
  };

}

struct XrWindow::Impl {
  // Instance
  XrInstance               instance = XR_NULL_HANDLE;
  XrSystemId               system   = XR_NULL_SYSTEM_ID;
  bool                     hasPanorama = false;
  bool                     hasEquirect2 = false;
  bool                     hasPassthrough     = false;
  bool                     hasDepth = false, hasAlphaBlend = false;
  bool                     lightEnabled = false, probePending = true, depthReady = false;
  bool                     surfaceCanProbe = false;
  bool                     permissionAsked = false, permissionGranted = false;
  bool                     permissionHintShown = false;
  bool                     traceSurface = CommandLine::inst().debugShow("surfacelight");
  int                      tracedSurfaceSetting = -1;
  unsigned                 tracedProbeGate = ~0u;
  XrTime                   nextPermissionCheck = 0;
  int64_t                  eyeColorFormat = VK_FORMAT_UNDEFINED;
  EnvironmentDepth         environmentDepth;
  SurfaceLight             surfaceLight;
  void invalidateSurface();
  void updateSurface();
  bool                     hasHandInteraction = false;
  bool                     hasPerfSettings    = false;
  bool                     hasSpaceWarp       = false;
  bool                     hasWindowFilter    = false;
  float                    refreshRate = 0.f;
  PFN_xrGetDisplayRefreshRateFB xrGetRate = nullptr;
  PFN_xrPerfSettingsSetPerformanceLevelEXT xrSetPerfLevel = nullptr;

  PFN_xrCreateVulkanInstanceKHR         xrCreateVulkanInstance      = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR     xrGetVulkanGraphicsDevice   = nullptr;
  PFN_xrCreateVulkanDeviceKHR           xrCreateVulkanDevice        = nullptr;
  PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanRequirements   = nullptr;
  PFN_xrPassthroughStartFB              xrStartPassthrough          = nullptr;
  PFN_xrGetSwapchainStateFB             xrGetSwapchainState         = nullptr;
  PFN_xrUpdateSwapchainFB               xrUpdateSwapchain           = nullptr;
  PFN_xrPassthroughPauseFB              xrPausePassthrough          = nullptr;
  PFN_xrCreatePassthroughFB             xrCreatePassthrough         = nullptr;
  PFN_xrDestroyPassthroughFB            xrDestroyPassthrough        = nullptr;
  PFN_xrCreatePassthroughLayerFB        xrCreatePassthroughLayer    = nullptr;
  PFN_xrDestroyPassthroughLayerFB       xrDestroyPassthroughLayer   = nullptr;

  PFN_xrPassthroughLayerSetStyleFB      xrSetPassthroughStyle      = nullptr;

  // Session
  VulkanNativeContext      vk;
  VkInstance               graphicsInstance = VK_NULL_HANDLE;
  VkExtent2D               rateTexel = {};
  XrTime                   retryRateAfter = 0;
  ShadingRateMap           lightingRate[Resources::MaxFramesInFlight];
  V3                       headForward;
  bool                     headValid = false;
  XrSession                session    = XR_NULL_HANDLE;
  XrSessionState           state      = XR_SESSION_STATE_UNKNOWN;
  bool                     running    = false;
  uint8_t                  transitionFailures = 0;
  XrSpace                  localSpace = XR_NULL_HANDLE;
  XrSpace                  viewSpace  = XR_NULL_HANDLE;
  bool                     passthroughUnavailable = false;
  XrPassthroughFB          passthrough      = XR_NULL_HANDLE;
  XrPassthroughLayerFB     passthroughLayer = XR_NULL_HANDLE;

  // Window image, one swapchain per eye
  uint32_t                 width  = 1600;
  uint32_t                 height = 900;
  uint32_t                 capacityW = 1600, capacityH = 900;
  XrSwapchain              swapchain[2] = {};
  Tempest::Device*         device = nullptr;
  // The game renders straight into the swapchain images through these
  std::vector<Attachment>  targets[2];
  int32_t                  acquired[2] = {-1, -1};

  // Application SpaceWarp
  uint32_t                 mvWidth = 0, mvHeight = 0;
  bool                     fullMotionExtent = false;
  XrSwapchain              mvSwapchain[2]    = {};
  XrSwapchain              depthSwapchain[2] = {};
  std::vector<Attachment>  mvTargets[2];
  std::vector<ZBuffer>     depthTargets[2];
  int32_t                  mvAcquired[2]    = {-1, -1};
  int32_t                  depthAcquired[2] = {-1, -1};
  bool                     spaceWarpWanted  = false;
  bool                     skipExtrapolation = true;
  bool                     scalePicture = true;
  XrPosef                  eyePose[2] = {};
  XrFovf                   eyeFov [2] = {};
  float                    eyeNear[2] = {};
  bool createSpaceWarpSwapchains();
  void destroySpaceWarpSwapchains();
  bool releaseSpaceWarp();
  // Window frame parts: grab handle (idle/hover/held) and resize corners (left/right, hover/held)
  XrSwapchain              handleSwapchain = XR_NULL_HANDLE;
  XrSwapchain              cornerSwapchain = XR_NULL_HANDLE;
  XrSwapchain              panorama = XR_NULL_HANDLE;
  XrSwapchain              environmentButton = XR_NULL_HANDLE;
  XrSwapchain              dimSlider = XR_NULL_HANDLE;
  static constexpr int     RailW = 160, KnobW = 24, SliderRows = 16;
  static constexpr float   DimLeft = 0.09f, DimTravel = 0.20f;
  bool                     sliderReady = false, dimDirty = false;
  bool                     dimStyleAvailable = false, dimUsesOpacity = false, grabTrigger = false;
  float                    passthroughDim = 0.f;
  XrTime                   lastDimHover = 0;
  uint32_t                 smokeFrame = 0;
  XrTime                   smokeCaptureUntil = 0;
  Tempest::Vec2            smokeWindow;
  static constexpr uint32_t SkyW = 2048, SkyH = SkyW/2, ButtonW = 128, ButtonH = 80;
  bool                     panoramaReady = false, buttonReady = false, immersive = false;
  bool                     staticImagesAttempted = false;
  bool                     handleReady = false, cornersReady = false;
  bool                     restoreSky = false;
  static constexpr uint32_t HandleW = 256, HandleH = 32, CornerSize = 64;

  // Frame
  XrFrameState             frame = {XR_TYPE_FRAME_STATE};
  bool                     frameBegun = false;
  bool                     eyesValid  = false;
  V3                       eyeInWindow[2];   // metres, window space: +x right, +y up, +z towards the viewer

  // Window placement (local space)
  XrPosef                  windowPose = {{0,0,0,1},{0,0,-1.5f}};
  float                    windowWidth = 2.4f, physicalHeight = 1.35f;
  int                      edgeStyle = 0;
  RenderPipeline           edgeOpaque, edgeCoverage, edgeMotion;
  float                    verticalFov = 67.5f;
  bool                     placed      = false;
  bool                     probePlaced = false;
  XrTime                   probeStableSince = 0;
  XrPosef                  probeAnchor{};
  std::vector<XrTime>      recenterTimes;
  int                      grabbedBy   = -1;
  Grip                     grabbed     = Grip::None;
  bool                     grabNear    = false;
  XrTime                   lastGrabTime = 0;
  V3                       grabLocal;          // grabbed point in the space of the holding pose
  V3                       grabWindow;         // same point in the window at the start of the grab
  Tempest::Vec2            grabExtent;
  V3                       headPos;

  // Input
  XrActionSet              actionSet  = XR_NULL_HANDLE;
  XrAction                 aAim       = XR_NULL_HANDLE;
  XrAction                 aNear      = XR_NULL_HANDLE;
  XrAction                 aGrab      = XR_NULL_HANDLE;
  XrAction                 aStick     = XR_NULL_HANDLE;
  XrAction                 aTrigger   = XR_NULL_HANDLE;
  XrAction                 aStickClick= XR_NULL_HANDLE;
  XrAction                 aA = XR_NULL_HANDLE, aB = XR_NULL_HANDLE, aX = XR_NULL_HANDLE, aY = XR_NULL_HANDLE;
  XrAction                 aMenu      = XR_NULL_HANDLE;
  std::array<Hand,2>       hands;
  PadState                 padState, releasePad;
  enum PadControl : size_t { StickLeft=PadState::Count, StickRight, TriggerLeft, TriggerRight, ControlCount };
  std::bitset<ControlCount> blockedControls;
  bool                     padInterrupted = false;

  ~Impl();

  bool createInstance();
  void createSession();
  void createSwapchains();
  void createPassthrough();
  void destroyPassthrough();
  void passthroughChanged(XrPassthroughStateChangedFlagsFB flags);
  bool roomAvailable() const { return passthroughLayer!=XR_NULL_HANDLE && !passthroughUnavailable; }
  void createActions();
  bool pollEvents();
  void stopSession();
  void updateSessionRunning();
  bool sessionResult(XrResult result, const char* operation);
  void updateInput();
  void cancelInput();
  void placeWindow();
  void fillStaticImages();
  void fillHandle(VkCommandBuffer cmd, VkFence fence);
  void fillCorners(VkCommandBuffer cmd, VkFence fence);
  void fillEnvironment(VkCommandBuffer cmd, VkFence fence);
  bool setEnvironment(bool useSky);
  void smokeHand(Hand& h);
  bool applyDim(float level);
  void saveDim();
  int sliderRow() const;
  void fillSlider(VkCommandBuffer cmd, VkFence fence);
  V3   buttonCenter() const { return handleCenter() + V3{0,-0.10f,0}; }
  Grip targetAt(float x, float y) const;
  V3   handleCenter() const;
  bool releaseEyes();
  void locateEyes();
  XrPath path(const char* name) const {
    XrPath value = XR_NULL_PATH;
    return ok(xrStringToPath(instance,name,&value),name) ? value : XR_NULL_PATH;
    }
  float  windowHeight() const { return physicalHeight; }
  float  edgeRadius() const { return edgeStyle==0 ? 0.f : 0.05f*std::min(windowWidth,physicalHeight); }
  float  edgeFeather() const { return edgeStyle==2 ? 0.026f*std::min(windowWidth,physicalHeight) : 0.f; }
  float  cornerInset() const { return 0.2928932f*edgeRadius()+0.3535534f*edgeFeather(); } // diagonal at 50% coverage
  static float portalDistance(float fov) {
    return 0.675f/std::tan(fov*float(M_PI)/360.f); // half the reference window height, in metres
    }
  };

static XrWindow* instance = nullptr;

// Where submit() and the eye targets spend their time (microseconds since the last report)
static uint64_t xrUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
  }
static uint64_t tAcquire = 0, tSwapWait = 0, tRelease = 0, tEndFrame = 0;

XrWindow::XrWindow() : impl(new Impl()) {
  }

XrWindow::~XrWindow() {
  if(instance==this) {
    vulkanCreateHooks() = {};
    instance = nullptr;
    }
  }

XrWindow* XrWindow::inst() {
  return instance;
  }

XrWindow::Impl::~Impl() {
  if(vk.device!=VK_NULL_HANDLE) {
    device->waitIdle();
    for(int eye=0; eye<2; ++eye) {
      targets[eye].clear();
      }
    }
  environmentDepth.reset();
  surfaceLight.reset();
  destroyPassthrough();
  if(panorama!=XR_NULL_HANDLE)
    xrDestroySwapchain(panorama);
  if(dimSlider!=XR_NULL_HANDLE)
    xrDestroySwapchain(dimSlider);
  if(environmentButton!=XR_NULL_HANDLE)
    xrDestroySwapchain(environmentButton);
  if(handleSwapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(handleSwapchain);
  if(cornerSwapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(cornerSwapchain);
  for(auto sw:swapchain)
    if(sw!=XR_NULL_HANDLE)
      xrDestroySwapchain(sw);
  destroySpaceWarpSwapchains();
  if(session!=XR_NULL_HANDLE)
    xrDestroySession(session);
  if(instance!=XR_NULL_HANDLE)
    xrDestroyInstance(instance);
  }

bool XrWindow::Impl::createInstance() {
  ANativeActivity* activity = AndroidApi::activity();
  if(activity==nullptr)
    return false;

  PFN_xrInitializeLoaderKHR initLoader = nullptr;
  if(!ok(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", reinterpret_cast<PFN_xrVoidFunction*>(&initLoader)), "loader entry") || initLoader==nullptr)
    return false;
  XrLoaderInitInfoAndroidKHR loaderInfo = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  loaderInfo.applicationVM      = activity->vm;
  loaderInfo.applicationContext = activity->clazz;
  if(!ok(initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)), "xrInitializeLoaderKHR"))
    return false;

  uint32_t count = 0;
  if(!ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "count instance extensions") || count==0)
    return false;
  std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
  if(!ok(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()), "enumerate instance extensions"))
    return false;
  props.resize(count);
  auto has = [&](const char* name) {
    for(auto& p:props)
      if(std::strcmp(p.extensionName,name)==0)
        return true;
    return false;
    };

  if(CommandLine::inst().isGpuProfile()) {
    // Foveation building blocks offered by the runtime (logged for profiling sessions)
    for(auto n:{"XR_FB_foveation", "XR_FB_foveation_vulkan", "XR_FB_foveation_configuration",
                "XR_META_foveation_eye_tracked", "XR_FB_swapchain_update_state"})
      Log::i("OpenXR: ", n, has(n) ? " available" : " not available");
    }

  std::vector<const char*> ext = {
    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
    };
  for(auto e:ext)
    if(!has(e)) {
      Log::e("OpenXR: missing ", e);
      return false;
      }
  hasEquirect2       = has(XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME);
  hasPanorama        = hasEquirect2 || has(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
  const bool canSetSkySampler = hasPanorama && has(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) &&
                                has(XR_FB_SWAPCHAIN_UPDATE_STATE_VULKAN_EXTENSION_NAME);
  if(canSetSkySampler) {
    ext.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    ext.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_VULKAN_EXTENSION_NAME);
    }
  if(hasPanorama)
    ext.push_back(hasEquirect2 ? XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME : XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
  hasPassthrough     = has(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  hasHandInteraction = has(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
  hasPerfSettings    = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  hasDepth = has(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME);
  hasAlphaBlend = has(XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME);
  if(hasDepth) {
    ext.push_back(XR_META_ENVIRONMENT_DEPTH_EXTENSION_NAME);
    if(hasAlphaBlend)
      ext.push_back(XR_FB_COMPOSITION_LAYER_ALPHA_BLEND_EXTENSION_NAME);
    }
  const bool hasRefreshRate = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  if(hasRefreshRate)
    ext.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
  hasSpaceWarp       = has(XR_FB_SPACE_WARP_EXTENSION_NAME);
  if(hasSpaceWarp)
    ext.push_back(XR_FB_SPACE_WARP_EXTENSION_NAME);
  hasWindowFilter = has(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  if(hasWindowFilter)
    ext.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
  if(hasPerfSettings)
    ext.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  if(hasPassthrough)
    ext.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  if(hasHandInteraction)
    ext.push_back(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);

  XrInstanceCreateInfoAndroidKHR android = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  android.applicationVM       = activity->vm;
  android.applicationActivity = activity->clazz;

  XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO};
  info.next = &android;
  std::strncpy(info.applicationInfo.applicationName, "OpenGothic Window3D", XR_MAX_APPLICATION_NAME_SIZE-1);
  std::strncpy(info.applicationInfo.engineName,      "OpenGothic",          XR_MAX_ENGINE_NAME_SIZE-1);
  info.applicationInfo.applicationVersion = 1;
  info.applicationInfo.engineVersion      = 1;
  info.applicationInfo.apiVersion         = XR_MAKE_VERSION(1, 0, 34);
  info.enabledExtensionCount = uint32_t(ext.size());
  info.enabledExtensionNames = ext.data();
  XrInstance created = XR_NULL_HANDLE;
  if(!ok(xrCreateInstance(&info, &created), "xrCreateInstance"))
    return false;
  instance = created;

  XrSystemGetInfo sys = {XR_TYPE_SYSTEM_GET_INFO};
  sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if(!ok(xrGetSystem(instance, &sys, &system), "xrGetSystem"))
    return false;

  if(hasDepth) {
    XrSystemEnvironmentDepthPropertiesMETA depthProps = {XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META};
    XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES,&depthProps};
    hasDepth = XR_SUCCEEDED(xrGetSystemProperties(instance,system,&props)) && depthProps.supportsEnvironmentDepth;
    }
  auto fn = [this](const char* name, auto& out) {
    PFN_xrVoidFunction proc = nullptr;
    if(!ok(xrGetInstanceProcAddr(instance, name, &proc), name) || proc==nullptr)
      return false;
    out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(proc);
    return true;
    };
  if(!fn("xrCreateVulkanInstanceKHR",          xrCreateVulkanInstance)    ||
     !fn("xrGetVulkanGraphicsDevice2KHR",      xrGetVulkanGraphicsDevice) ||
     !fn("xrCreateVulkanDeviceKHR",            xrCreateVulkanDevice)      ||
     !fn("xrGetVulkanGraphicsRequirements2KHR",xrGetVulkanRequirements))
    return false;
  if(hasPerfSettings)
    hasPerfSettings = fn("xrPerfSettingsSetPerformanceLevelEXT", xrSetPerfLevel);
  if(hasRefreshRate)
    fn("xrGetDisplayRefreshRateFB",xrGetRate);
  if(canSetSkySampler) {
    fn("xrGetSwapchainStateFB", xrGetSwapchainState);
    fn("xrUpdateSwapchainFB", xrUpdateSwapchain);
    }
  if(hasPassthrough) {
    hasPassthrough = fn("xrPassthroughStartFB",        xrStartPassthrough)       &&
                     fn("xrPassthroughPauseFB",        xrPausePassthrough)       &&
                     fn("xrCreatePassthroughFB",       xrCreatePassthrough)      &&
                     fn("xrDestroyPassthroughFB",      xrDestroyPassthrough)     &&
                     fn("xrCreatePassthroughLayerFB",  xrCreatePassthroughLayer) &&
                     fn("xrDestroyPassthroughLayerFB", xrDestroyPassthroughLayer);
    if(hasPassthrough)
      fn("xrPassthroughLayerSetStyleFB", xrSetPassthroughStyle);
    }

  // Required before the device is created
  XrGraphicsRequirementsVulkan2KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  if(!ok(xrGetVulkanRequirements(instance, system, &req), "Vulkan graphics requirements"))
    return false;

  auto& hooks   = vulkanCreateHooks();
  hooks.context = this;
  hooks.createInstance = [](void* ctx, const VkInstanceCreateInfo* ci, VkInstance* out) -> VkResult {
    auto& self = *static_cast<Impl*>(ctx);
    XrVulkanInstanceCreateInfoKHR info = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    info.systemId               = self.system;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanCreateInfo       = ci;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if(!ok(self.xrCreateVulkanInstance(self.instance, &info, out, &vr), "xrCreateVulkanInstanceKHR"))
      return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
    };
  hooks.physicalDevice = [](void* ctx, VkInstance vkInstance) -> VkPhysicalDevice {
    auto& self = *static_cast<Impl*>(ctx);
    XrVulkanGraphicsDeviceGetInfoKHR info = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId       = self.system;
    info.vulkanInstance = vkInstance;
    self.graphicsInstance = vkInstance;
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    ok(self.xrGetVulkanGraphicsDevice(self.instance, &info, &pdev), "xrGetVulkanGraphicsDevice2KHR");
    return pdev;
    };
  hooks.createDevice = [](void* ctx, VkPhysicalDevice pdev, const VkDeviceCreateInfo* ci, VkDevice* out) -> VkResult {
    auto& self = *static_cast<Impl*>(ctx);
    XrVulkanDeviceCreateInfoKHR info = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    info.systemId               = self.system;
    info.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    info.vulkanPhysicalDevice   = pdev;
    info.vulkanCreateInfo       = ci;
    if(CommandLine::inst().isGpuProfile()) {
      // Variable rate shading support (foveation research), logged for profiling sessions
      VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
      VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      f2.pNext = &fsr;
      vkGetPhysicalDeviceFeatures2(pdev, &f2);
      VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
      p2.pNext = &fsrp;
      vkGetPhysicalDeviceProperties2(pdev, &p2);
      Log::i("Vulkan VRS: pipeline ", int(fsr.pipelineFragmentShadingRate), " primitive ", int(fsr.primitiveFragmentShadingRate),
             " attachment ", int(fsr.attachmentFragmentShadingRate), " texel ", fsrp.minFragmentShadingRateAttachmentTexelSize.width,
             "..", fsrp.maxFragmentShadingRateAttachmentTexelSize.width, " max fragment ", fsrp.maxFragmentSize.width, "x",
             fsrp.maxFragmentSize.height);
      }
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    self.rateTexel = ShadingRateMap::supported(self.graphicsInstance,pdev,*ci);
    if(self.rateTexel.width!=0) {
      VkDeviceCreateInfo copy = *ci;
      std::vector<const char*> names(ci->ppEnabledExtensionNames,ci->ppEnabledExtensionNames+ci->enabledExtensionCount);
      if(std::none_of(names.begin(),names.end(),[](const char* name){ return std::strcmp(name,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)==0; }))
        names.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
      copy.enabledExtensionCount = uint32_t(names.size());
      copy.ppEnabledExtensionNames = names.data();
      VkPhysicalDeviceFragmentShadingRateFeaturesKHR enable = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
      enable.pipelineFragmentShadingRate = enable.attachmentFragmentShadingRate = VK_TRUE;
      enable.pNext = const_cast<void*>(ci->pNext);
      copy.pNext = &enable;
      info.vulkanCreateInfo = &copy;
      if(XR_SUCCEEDED(self.xrCreateVulkanDevice(self.instance,&info,out,&vr)) && vr==VK_SUCCESS) {
        Log::i("OpenXR: lighting shading-rate attachment available, texels ",self.rateTexel.width,"x",self.rateTexel.height);
        return vr;
        }
      Log::i("OpenXR: optional lighting shading rate unavailable, retrying device creation");
      self.rateTexel = {};
      info.vulkanCreateInfo = ci;
      }
    if(!ok(self.xrCreateVulkanDevice(self.instance, &info, out, &vr), "xrCreateVulkanDeviceKHR"))
      return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
    };
  return true;
  }

bool XrWindow::preInit() {
  if(instance!=nullptr)
    return true;
  std::unique_ptr<XrWindow> w(new XrWindow());
  if(!w->impl->createInstance()) {
    vulkanCreateHooks() = VulkanCreateHooks();
    Log::i("OpenXR: not available, using the Android window");
    return false;
    }
  instance = w.release();
  return true;
  }

void XrWindow::attach(Device& device) {
  impl->vk     = vulkanNativeContext(device);
  impl->device = &device;
  impl->windowWidth = CommandLine::inst().xrWindowWidth();
  impl->physicalHeight = CommandLine::inst().xrWindowHeight();
  impl->createSession();
  AndroidApi::setExternalPresentation(int(impl->width), int(impl->height), uiScale());
  }

void XrWindow::Impl::createSession() {
  XrGraphicsBindingVulkan2KHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance         = vk.instance;
  binding.physicalDevice   = vk.physicalDevice;
  binding.device           = vk.device;
  binding.queueFamilyIndex = vk.queueFamily;
  binding.queueIndex       = 0;

  XrSessionCreateInfo info = {XR_TYPE_SESSION_CREATE_INFO};
  info.next     = &binding;
  info.systemId = system;
  XrSession created = XR_NULL_HANDLE;
  if(!ok(xrCreateSession(instance, &info, &created), "xrCreateSession"))
    throw std::runtime_error("OpenXR session");
  session = created;

  XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
  space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if(!ok(xrCreateReferenceSpace(session, &space, &localSpace), "local space"))
    throw std::runtime_error("OpenXR local space");
  space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
  if(!ok(xrCreateReferenceSpace(session, &space, &viewSpace), "view space"))
    throw std::runtime_error("OpenXR view space");

  createSwapchains();
  if(hasSpaceWarp) {
    bool ready = false;
    try {
      ready = createSpaceWarpSwapchains();
      }
    catch(const std::bad_alloc&) {}
    catch(const std::system_error& e) {
      if(e.code()!=GraphicsErrc::OutOfVideoMemory && e.code()!=GraphicsErrc::OutOfHostMemory)
        throw;
      }
    if(!ready) {
      destroySpaceWarpSwapchains();
      Log::e("OpenXR: SpaceWarp targets unavailable, using ordinary stereo");
      }
    }
  createPassthrough();
  createActions();
  }

static std::vector<XrSwapchainImageVulkan2KHR> swapchainImages(XrSwapchain chain) {
  uint32_t count = 0;
  if(!ok(xrEnumerateSwapchainImages(chain, 0, &count, nullptr), "count swapchain images") || count==0)
    return {};
  std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  if(!ok(xrEnumerateSwapchainImages(chain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "enumerate swapchain images"))
    return {};
  images.resize(count);
  return images;
  }

static std::vector<int64_t> swapchainFormats(XrSession session) {
  uint32_t count = 0;
  if(!ok(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "count swapchain formats") || count==0)
    return {};
  std::vector<int64_t> formats(count);
  if(!ok(xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "enumerate swapchain formats"))
    return {};
  formats.resize(count);
  return formats;
  }

static XrSwapchain makeSwapchain(XrSession session, const XrSwapchainCreateInfo& info, const char* name) {
  XrSwapchain chain = XR_NULL_HANDLE;
  return ok(xrCreateSwapchain(session, &info, &chain), name) ? chain : XR_NULL_HANDLE;
  }

void XrWindow::Impl::createSwapchains() {
  const auto formats = swapchainFormats(session);

  // The renderer and static uploads both require four RGBA8 channels.
  auto supported = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f)!=formats.end(); };
  const int64_t format = supported(VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  eyeColorFormat = format;
  if(!supported(format))
    throw std::runtime_error("OpenXR requires an RGBA8 window swapchain");

  XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  // Mutable format: the game writes its gamma-encoded RGBA8 through a UNORM view of the sRGB image
  info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
  if(CommandLine::inst().xrCaptureInterval()>0)
    info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT; // -capture reads the eye images back
  info.format      = format;
  info.sampleCount = 1;
  info.faceCount   = 1;
  info.arraySize   = 1;
  info.mipCount    = 1;
  XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
  if(ok(xrGetSystemProperties(instance,system,&props),"system image limits")) {
    capacityW = std::min(2048u,props.graphicsProperties.maxSwapchainImageWidth);
    capacityH = std::min(1536u,props.graphicsProperties.maxSwapchainImageHeight);
    }
  for(const auto& value:CommandLine::inst().settingsOverrides()) {
    if(value.section=="BENCHMARK" && value.key=="xrTightSwapchains" && value.value=="1") {
      capacityW = std::min(capacityW,width);
      capacityH = std::min(capacityH,height);
      Log::i("XR trial eye allocation ",capacityW,"x",capacityH);
      }
    }
  auto makeEyes = [&] {
    info.width = capacityW;
    info.height = capacityH;
    for(int eye=0;eye<2;++eye) {
      swapchain[eye] = makeSwapchain(session,info,"window swapchain");
      if(swapchain[eye]==XR_NULL_HANDLE)
        return false;
      auto images = swapchainImages(swapchain[eye]);
      if(images.empty())
        return false;
      for(auto& im:images)
        targets[eye].push_back(vulkanBorrowColorAttachment(*device,im.image,capacityW,capacityH));
      }
    return true;
    };
  bool ready = false;
  try { ready = makeEyes(); }
  catch(const std::bad_alloc&) {}
  catch(const std::system_error& e) {
    if(e.code()!=GraphicsErrc::OutOfVideoMemory && e.code()!=GraphicsErrc::OutOfHostMemory)
      throw;
    }
  if(!ready) {
    for(int eye=0;eye<2;++eye) {
      targets[eye].clear();
      if(swapchain[eye]!=XR_NULL_HANDLE)
        xrDestroySwapchain(std::exchange(swapchain[eye],XR_NULL_HANDLE));
      }
    if(capacityW<=width && capacityH<=height)
      throw std::runtime_error("OpenXR window images");
    capacityW=std::min(capacityW,width);
    capacityH=std::min(capacityH,height);
    if(!makeEyes())
      throw std::runtime_error("OpenXR window images");
    }
  width = std::min(width,capacityW);
  height = std::min(height,capacityH);

  info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.width       = HandleW;
  info.height      = HandleH;
  info.arraySize   = 3;
  handleSwapchain = makeSwapchain(session, info, "handle swapchain");
  info.width       = CornerSize;
  info.height      = CornerSize;
  info.arraySize   = 4;
  cornerSwapchain = makeSwapchain(session, info, "corner swapchain");

  if(hasPanorama) {
    info.width = SkyW;
    info.height = SkyH;
    info.arraySize = 1;
    info.usageFlags |= XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    panorama = makeSwapchain(session, info, "static environment swapchain");
    if(panorama!=XR_NULL_HANDLE) {
      if(xrGetSwapchainState && xrUpdateSwapchain) {
        XrSwapchainStateSamplerVulkanFB sampler = {XR_TYPE_SWAPCHAIN_STATE_SAMPLER_VULKAN_FB};
        auto* state = reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&sampler);
        if(ok(xrGetSwapchainState(panorama, state), "read sky sampler")) {
          // Longitude wraps at the panorama seam; each pole uses its edge texel.
          sampler.wrapModeS = VK_SAMPLER_ADDRESS_MODE_REPEAT;
          sampler.wrapModeT = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
          ok(xrUpdateSwapchain(panorama, state), "set sky sampler");
          }
        }
      info.width = ButtonW;
      info.height = ButtonH;
      info.arraySize = 4; // room / sky icons, each idle or hovered
      environmentButton = makeSwapchain(session, info, "environment button swapchain");
      if(environmentButton && xrSetPassthroughStyle) {
        info.width = RailW+KnobW;
        info.height = KnobW*SliderRows;
        info.arraySize = 1;
        dimSlider = makeSwapchain(session, info, "dimming slider swapchain");
        }
      }
    }
  }

bool XrWindow::Impl::createSpaceWarpSwapchains() {
  // Motion vectors at a quarter of the window resolution (the runtime upsamples them)
  mvWidth  = std::max(width/4u,  16u);
  mvHeight = std::max(height/4u, 16u);
  // XR session setup precedes Gothic.ini loading. Only an explicit command-line
  // experiment asks for the runtime's full recommended images at every window size.
  for(const auto& value:CommandLine::inst().settingsOverrides()) {
    if(value.section=="BENCHMARK" && value.key=="xrMotionExtent")
      fullMotionExtent = value.value=="1";
    }
  if(fullMotionExtent) {
    XrSystemSpaceWarpPropertiesFB warp{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES,&warp};
    if(!ok(xrGetSystemProperties(instance,system,&props),"SpaceWarp recommended extent") ||
       warp.recommendedMotionVectorImageRectWidth==0 || warp.recommendedMotionVectorImageRectHeight==0)
      return false;
    mvWidth = warp.recommendedMotionVectorImageRectWidth;
    mvHeight = warp.recommendedMotionVectorImageRectHeight;
    Log::i("XR motion-vector trial full recommended extent ",mvWidth,"x",mvHeight);
    }

  const auto formats = swapchainFormats(session);
  auto supported = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f)!=formats.end(); };
  if(!supported(VK_FORMAT_R16G16B16A16_SFLOAT) || !supported(VK_FORMAT_D32_SFLOAT)) {
    Log::e("OpenXR: SpaceWarp needs RGBA16F and D32F swapchains");
    return false;
    }

  const auto& cli = CommandLine::inst();
  const XrSwapchainUsageFlags readback = cli.xrCaptureBurst()>0 && cli.debugShow("warpraw") ?
                                         XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0;
  for(int eye=0; eye<2; ++eye) {
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | readback;
    info.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.sampleCount = 1;
    info.width       = fullMotionExtent ? mvWidth : capacityW/4;
    info.height      = fullMotionExtent ? mvHeight : capacityH/4;
    info.faceCount   = 1;
    info.arraySize   = 1;
    info.mipCount    = 1;
    mvSwapchain[eye] = makeSwapchain(session, info, "motion vector swapchain");
    if(mvSwapchain[eye]==XR_NULL_HANDLE)
      return false;
    info.usageFlags  = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | readback;
    info.format      = VK_FORMAT_D32_SFLOAT;
    depthSwapchain[eye] = makeSwapchain(session, info, "depth swapchain");
    if(depthSwapchain[eye]==XR_NULL_HANDLE)
      return false;
    for(auto& i:swapchainImages(mvSwapchain[eye]))
      mvTargets[eye].push_back(vulkanBorrowColorAttachment(*device, i.image, info.width, info.height, TextureFormat::RGBA16F));
    for(auto& i:swapchainImages(depthSwapchain[eye]))
      depthTargets[eye].push_back(vulkanBorrowDepthAttachment(*device, i.image, info.width, info.height, TextureFormat::Depth32F));
    if(mvTargets[eye].empty() || depthTargets[eye].empty())
      return false;
    }
  Log::i("OpenXR: SpaceWarp available, motion vectors ", mvWidth, "x", mvHeight);
  return true;
  }

void XrWindow::Impl::destroySpaceWarpSwapchains() {
  hasSpaceWarp = false;
  for(int eye=0; eye<2; ++eye) {
    mvTargets[eye].clear();
    depthTargets[eye].clear();
    for(auto chain : {&mvSwapchain[eye], &depthSwapchain[eye]}) {
      if(*chain!=XR_NULL_HANDLE)
        xrDestroySwapchain(*chain);
      *chain = XR_NULL_HANDLE;
      }
    }
  }

static bool releaseImage(XrSwapchain chain, int32_t& index) {
  if(index<0)
    return true;
  index = -1;
  XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  return ok(xrReleaseSwapchainImage(chain, &rel), "release swapchain image");
  }

bool XrWindow::Impl::releaseSpaceWarp() {
  bool released = true;
  for(int eye=0; eye<2; ++eye) {
    released &= releaseImage(mvSwapchain[eye], mvAcquired[eye]);
    released &= releaseImage(depthSwapchain[eye], depthAcquired[eye]);
    }
  return released;
  }

void XrWindow::Impl::createPassthrough() {
  if(!hasPassthrough)
    return;
  XrPassthroughCreateInfoFB info = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  info.flags = immersive ? 0 : XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  XrPassthroughFB feature = XR_NULL_HANDLE;
  if(!ok(xrCreatePassthrough(session, &info, &feature), "xrCreatePassthroughFB"))
    return;
  XrPassthroughLayerCreateInfoFB layer = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  layer.passthrough = feature;
  layer.flags       = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  layer.purpose     = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
  XrPassthroughLayerFB room = XR_NULL_HANDLE;
  if(!ok(xrCreatePassthroughLayer(session, &layer, &room), "xrCreatePassthroughLayerFB")) {
    xrDestroyPassthrough(feature);
    return;
    }
  passthrough = feature;
  passthroughLayer = room;
  dimStyleAvailable = xrSetPassthroughStyle!=nullptr;
  dimUsesOpacity = false;
  if(sliderReady && passthroughDim>0.f)
    applyDim(passthroughDim);
  }

bool XrWindow::Impl::applyDim(float level) {
  if(!dimStyleAvailable || passthroughLayer==XR_NULL_HANDLE)
    return false;
  // The two CIELAB lightness adjustments together give Lout = scale * Lin.
  const float scale = 1.f-0.94f*level;
  XrPassthroughBrightnessContrastSaturationFB color = {XR_TYPE_PASSTHROUGH_BRIGHTNESS_CONTRAST_SATURATION_FB};
  color.brightness = 50.f*(scale-1.f);
  color.contrast = scale;
  color.saturation = scale;
  XrPassthroughStyleFB style = {XR_TYPE_PASSTHROUGH_STYLE_FB};
  style.textureOpacityFactor = dimUsesOpacity ? scale : 1.f;
  style.next = level>0.f && !dimUsesOpacity ? &color : nullptr;
  auto result = xrSetPassthroughStyle(passthroughLayer,&style);
  if(style.next!=nullptr && (result==XR_ERROR_FEATURE_UNSUPPORTED || result==XR_ERROR_VALIDATION_FAILURE)) {
    // Older passthrough runtimes can still fade the bottom layer against black.
    style.next = nullptr;
    style.textureOpacityFactor = scale;
    result = xrSetPassthroughStyle(passthroughLayer,&style);
    if(XR_SUCCEEDED(result)) {
      dimUsesOpacity = true;
      Log::i("OpenXR: using passthrough opacity for room dimming");
      }
    }
  if(!ok(result, "style passthrough")) {
    dimStyleAvailable = false;
    return false;
    }
  passthroughDim = level;
  return true;
  }

void XrWindow::Impl::saveDim() {
  if(std::exchange(dimDirty,false)) {
    Gothic::settingsSetF("INTERNAL","xrPassthroughDim",passthroughDim);
    Gothic::flushSettings();
    }
  }

int XrWindow::Impl::sliderRow() const {
  if(!sliderReady || !dimStyleAvailable || !roomAvailable() || lastDimHover==0)
    return -1;
  // Keep the slider within reach when a ray crosses the gap beside the button.
  const XrDuration age = frame.predictedDisplayTime-lastDimHover;
  const auto fade = std::max<XrDuration>(0,age-600'000'000);
  if(fade>=250'000'000)
    return -1;
  return SliderRows-1-int(fade*SliderRows/250'000'000);
  }

void XrWindow::Impl::destroyPassthrough() {
  if(passthroughLayer!=XR_NULL_HANDLE)
    xrDestroyPassthroughLayer(passthroughLayer);
  if(passthrough!=XR_NULL_HANDLE)
    xrDestroyPassthrough(passthrough);
  passthroughLayer = XR_NULL_HANDLE;
  passthrough = XR_NULL_HANDLE;
  }

void XrWindow::Impl::passthroughChanged(XrPassthroughStateChangedFlagsFB flags) {
  Log::i("OpenXR: passthrough state flags ", flags);
  if(flags & XR_PASSTHROUGH_STATE_CHANGED_NON_RECOVERABLE_ERROR_BIT_FB) {
    destroyPassthrough();
    passthroughUnavailable = true;
    return;
    }
  const bool reinit = (flags & XR_PASSTHROUGH_STATE_CHANGED_REINIT_REQUIRED_BIT_FB)!=0;
  const bool restored = (flags & XR_PASSTHROUGH_STATE_CHANGED_RESTORED_ERROR_BIT_FB)!=0;
  if(reinit)
    destroyPassthrough();
  if(reinit || restored) {
    if(passthroughLayer==XR_NULL_HANDLE)
      createPassthrough();
    passthroughUnavailable = false;
    }
  if(!restored && (flags & XR_PASSTHROUGH_STATE_CHANGED_RECOVERABLE_ERROR_BIT_FB)) {
    passthroughUnavailable = true;
    }
  if((reinit || restored) && restoreSky && roomAvailable())
    restoreSky = !setEnvironment(true);
  }

void XrWindow::Impl::createActions() {
  XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(setInfo.actionSetName,          "game");
  std::strcpy(setInfo.localizedActionSetName, "Game");
  XrActionSet set = XR_NULL_HANDLE;
  if(!ok(xrCreateActionSet(instance,&setInfo,&set), "action set"))
    return;

  hands[0].path = path("/user/hand/left");
  hands[1].path = path("/user/hand/right");
  XrPath handPaths[2];
  uint32_t handCount = 0;
  for(const auto& h:hands)
    if(h.path!=XR_NULL_PATH)
      handPaths[handCount++] = h.path;

  auto make = [&](XrAction& out, const char* name, const char* label, XrActionType type, bool perHand) {
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strcpy(info.actionName,          name);
    std::strcpy(info.localizedActionName, label);
    if(perHand) {
      if(handCount==0)
        return;
      info.countSubactionPaths = handCount;
      info.subactionPaths      = handPaths;
      }
    XrAction action = XR_NULL_HANDLE;
    if(ok(xrCreateAction(set,&info,&action),name))
      out = action;
    };
  make(aAim,        "aim",          "Aim",              XR_ACTION_TYPE_POSE_INPUT,     true);
  make(aNear,       "touch",        "Touch point",      XR_ACTION_TYPE_POSE_INPUT,     true);
  make(aGrab,       "grab_window",  "Move the window",  XR_ACTION_TYPE_FLOAT_INPUT,    true);
  make(aStick,      "stick",        "Thumbstick",       XR_ACTION_TYPE_VECTOR2F_INPUT, true);
  make(aTrigger,    "trigger",      "Trigger",          XR_ACTION_TYPE_FLOAT_INPUT,    true);
  make(aStickClick, "stick_click",  "Thumbstick press", XR_ACTION_TYPE_BOOLEAN_INPUT,  true);
  make(aA,          "button_a",     "A",                XR_ACTION_TYPE_BOOLEAN_INPUT,  false);
  make(aB,          "button_b",     "B",                XR_ACTION_TYPE_BOOLEAN_INPUT,  false);
  make(aX,          "button_x",     "X",                XR_ACTION_TYPE_BOOLEAN_INPUT,  false);
  make(aY,          "button_y",     "Y",                XR_ACTION_TYPE_BOOLEAN_INPUT,  false);
  make(aMenu,       "menu",         "Menu",             XR_ACTION_TYPE_BOOLEAN_INPUT,  false);

  auto suggest = [&](const char* profile, std::initializer_list<std::pair<XrAction,const char*>> list) {
    const auto profilePath = path(profile);
    if(profilePath==XR_NULL_PATH)
      return;
    std::vector<XrActionSuggestedBinding> b;
    for(auto [action,component]:list) {
      if(action==XR_NULL_HANDLE)
        continue;
      const auto hand = std::string_view(component).starts_with("/user/hand/left/") ? 0u : 1u;
      if(hands[hand].path==XR_NULL_PATH)
        continue;
      if(const auto binding = path(component); binding!=XR_NULL_PATH)
        b.push_back({action,binding});
      }
    if(b.empty())
      return;
    XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile     = profilePath;
    s.countSuggestedBindings = uint32_t(b.size());
    s.suggestedBindings      = b.data();
    ok(xrSuggestInteractionProfileBindings(instance, &s), profile);
    };
  suggest("/interaction_profiles/oculus/touch_controller", {
    {aAim,        "/user/hand/left/input/aim/pose"},
    {aAim,        "/user/hand/right/input/aim/pose"},
    {aNear,       "/user/hand/left/input/grip/pose"},
    {aNear,       "/user/hand/right/input/grip/pose"},
    {aGrab,       "/user/hand/left/input/squeeze/value"},
    {aGrab,       "/user/hand/right/input/squeeze/value"},
    {aStick,      "/user/hand/left/input/thumbstick"},
    {aStick,      "/user/hand/right/input/thumbstick"},
    {aTrigger,    "/user/hand/left/input/trigger/value"},
    {aTrigger,    "/user/hand/right/input/trigger/value"},
    {aStickClick, "/user/hand/left/input/thumbstick/click"},
    {aStickClick, "/user/hand/right/input/thumbstick/click"},
    {aA,          "/user/hand/right/input/a/click"},
    {aB,          "/user/hand/right/input/b/click"},
    {aX,          "/user/hand/left/input/x/click"},
    {aY,          "/user/hand/left/input/y/click"},
    {aMenu,       "/user/hand/left/input/menu/click"},
    });
  if(hasHandInteraction) {
    suggest("/interaction_profiles/ext/hand_interaction_ext", {
      {aAim,  "/user/hand/left/input/aim/pose"},
      {aAim,  "/user/hand/right/input/aim/pose"},
      {aNear, "/user/hand/left/input/pinch_ext/pose"},
      {aNear, "/user/hand/right/input/pinch_ext/pose"},
      {aGrab, "/user/hand/left/input/pinch_ext/value"},
      {aGrab, "/user/hand/right/input/pinch_ext/value"},
      });
    }

  XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets      = &set;
  if(!ok(xrAttachSessionActionSets(session,&attach), "attach action sets")) {
    xrDestroyActionSet(set);
    return;
    }
  actionSet = set;

  auto space = [&](XrAction action, XrPath hand, const char* name) -> XrSpace {
    if(action==XR_NULL_HANDLE || hand==XR_NULL_PATH)
      return XR_NULL_HANDLE;
    XrActionSpaceCreateInfo info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action            = action;
    info.subactionPath     = hand;
    info.poseInActionSpace = {{0,0,0,1},{0,0,0}};
    XrSpace result = XR_NULL_HANDLE;
    return ok(xrCreateActionSpace(session,&info,&result),name) ? result : XR_NULL_HANDLE;
    };
  for(auto& h:hands) {
    h.aimSpace  = space(aAim,h.path,"aim space");
    h.nearSpace = space(aNear,h.path,"touch space");
    }
  }

void XrWindow::Impl::stopSession() {
  state = XR_SESSION_STATE_EXITING;
  running = false;
  cancelInput();
  SystemApi::exit();
  }

bool XrWindow::Impl::sessionResult(XrResult result, const char* operation) {
  if(result==XR_ERROR_INSTANCE_LOST || result==XR_ERROR_SESSION_LOST)
    stopSession();
  return ok(result, operation);
  }

void XrWindow::Impl::updateSessionRunning() {
  const bool begin = state==XR_SESSION_STATE_READY && !running;
  const bool end = state==XR_SESSION_STATE_STOPPING && running;
  if(!begin && !end)
    return;

  XrSessionBeginInfo info = {XR_TYPE_SESSION_BEGIN_INFO};
  info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  const XrResult result = begin ? xrBeginSession(session, &info) : xrEndSession(session);
  if(!sessionResult(result, begin ? "xrBeginSession" : "xrEndSession")) {
    // The runtime may stay READY or STOPPING without sending a second event.
    if(state!=XR_SESSION_STATE_EXITING && ++transitionFailures>=3)
      stopSession();
    return;
    }
  transitionFailures = 0;
  running = begin;
  if(begin && xrGetRate && XR_FAILED(xrGetRate(session,&refreshRate)))
    refreshRate = 0.f;
  if(begin && hasPerfSettings) {
    const bool low = CommandLine::inst().xrPerfLevel()=="low";
    const auto lvl = low ? XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
    ok(xrSetPerfLevel(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, lvl), "CPU level");
    ok(xrSetPerfLevel(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, lvl), "GPU level");
    }
  }

bool XrWindow::Impl::pollEvents() {
  bool interrupted = false;
  bool attempted = false;
  while(state!=XR_SESSION_STATE_EXITING) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult result = xrPollEvent(instance, &ev);
    if(result!=XR_SUCCESS) {
      if(result!=XR_EVENT_UNAVAILABLE)
        sessionResult(result, "xrPollEvent");
      break;
      }
    switch(ev.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
        if(e.session!=session)
          break;
        if(state!=e.state)
          transitionFailures = 0;
        interrupted |= (state==XR_SESSION_STATE_FOCUSED && e.state!=XR_SESSION_STATE_FOCUSED);
        state = e.state;
        if(state!=XR_SESSION_STATE_FOCUSED)
          cancelInput();
        Log::i("OpenXR: session state ", int(state));
        if(state==XR_SESSION_STATE_READY || state==XR_SESSION_STATE_STOPPING) {
          updateSessionRunning();
          attempted = true;
          }
        else if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING) {
          stopSession();
          }
        break;
        }
      case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
        refreshRate = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB&>(ev).toDisplayRefreshRate;
        break;
      case XR_TYPE_EVENT_DATA_PASSTHROUGH_STATE_CHANGED_FB:
        passthroughChanged(reinterpret_cast<const XrEventDataPassthroughStateChangedFB&>(ev).flags);
        break;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
        auto& change = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
        if(change.session==session && change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL)
          recenterTimes.push_back(change.changeTime);
        break;
        }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        stopSession();
        break;
      default:
        break;
      }
    }
  if(!attempted && transitionFailures!=0)
    updateSessionRunning();
  return interrupted;
  }

void XrWindow::Impl::placeWindow() {
  const auto due = [this](XrTime t) { return t<=frame.predictedDisplayTime; };
  if(std::erase_if(recenterTimes,due)!=0) {
    placed = false;
    if(probePlaced) {
      probePlaced = false;
      probeStableSince = 0;
      Log::i("XR bench placement reset by LOCAL space change");
      }
    cancelInput();
    }
  skipExtrapolation = !placed;
  headValid = false;
  XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
  if(!XR_SUCCEEDED(xrLocateSpace(viewSpace, localSpace, frame.predictedDisplayTime, &head))) {
    probeStableSince = 0;
    return;
    }
  const bool valid = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                     (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
  if(!valid) {
    probeStableSince = 0;
    return;
    }
  headValid = true;
  headForward = rotate(head.pose.orientation,V3{0,0,-1});
  headPos = toV3(head.pose.position);
  if(CommandLine::inst().xrFaceView() && !probePlaced) {
    // The unattended headset can rest on its side. Wait for tracking to settle
    // after world load, place the bench scene along the gaze ray once, and leave it fixed.
    const auto& a = probeAnchor.orientation;
    const auto& b = head.pose.orientation;
    const float cosHalfAngle = std::abs(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w);
    // A stationary desk headset can report valid inferred tracking. The gaze
    // fixture only needs a stable valid pose in the submitted LOCAL space.
    if(state!=XR_SESSION_STATE_FOCUSED ||
       Gothic::inst().checkLoading()!=Gothic::LoadState::Idle || !Gothic::inst().player()) {
      probeStableSince = 0;
      }
    else if(!probeStableSince || cosHalfAngle<0.999f || length(headPos-toV3(probeAnchor.position))>0.04f) {
      probeStableSince = frame.predictedDisplayTime;
      probeAnchor = head.pose;
      }
    else if(frame.predictedDisplayTime-probeStableSince>=500000000) {
      const float requested = Gothic::settingsGetF("BENCHMARK","xrDistance");
      const float distance = requested>0.f ? std::clamp(requested,0.3f,4.f) :
                                             std::clamp(portalDistance(verticalFov),0.6f,3.f);
      const float side = std::clamp(Gothic::settingsGetF("BENCHMARK","xrOffsetX"),-2.f,2.f);
      windowPose = head.pose;
      windowPose.position = toXr(headPos+rotate(head.pose.orientation,{side,0,-distance}));
      probePlaced = placed = skipExtrapolation = true;
      invalidateSurface();
      Log::i("XR bench window placed on gaze, distance=",distance," side=",side," flags=",uint32_t(head.locationFlags),
             " forward=",headForward.x,",",headForward.y,",",headForward.z);
      }
    }
  if(placed)
    return;

  V3 fwd = headForward;
  fwd.y  = 0;
  fwd    = normalize(fwd);
  // The sweet spot: the window covers exactly the game's field of view, so nothing is cropped
  const float distance = std::clamp(portalDistance(verticalFov), 0.6f, 3.f);
  const V3    pos      = headPos + fwd*std::sqrt(distance*distance-0.1f*0.1f) + V3{0,-0.1f,0};
  windowPose.position    = toXr(pos);
  windowPose.orientation = facing(headPos - pos);
  placed = true;
  invalidateSurface();
  }

void XrWindow::Impl::cancelInput() {
  invalidateSurface();
  saveDim();
  lastDimHover = 0;
  padInterrupted = true;
  blockedControls.set();
  releasePad = {};
  padState = {};
  grabbedBy = -1;
  grabbed = Grip::None;
  for(auto& h:hands) {
    h.hover = Grip::None;
    h.triggerHeld = h.grabHeld = true; // require release before a new grab
    }
  }

// Repeat the same physical inputs through the normal hit-test and capture path.
// Only -xrsmoke enables this sequence. Frames count after the save has loaded.
void XrWindow::Impl::smokeHand(Hand& hand) {
  if(!frame.shouldRender || !headValid || !placed || !buttonReady ||
      !Gothic::inst().world() || !Gothic::inst().player() || Gothic::inst().checkLoading()!=Gothic::LoadState::Idle)
    return;
  ++smokeFrame;
  const uint32_t f = smokeFrame;
  if(f==1) {
    smokeWindow = Tempest::Vec2(windowWidth,physicalHeight);
    Log::i("XR smoke room: world loaded, passthrough=",!immersive);
    }
  // Hold each inspection pose for several seconds at half refresh.
  if(f==111)
    Log::i("XR smoke sky: immersive=",immersive);
  if(f==295)
    Log::i("XR smoke passthrough: active=",!immersive);
  if(f==470)
    Log::i("XR smoke dim: level=",passthroughDim," sliderRow=",sliderRow());
  if(f==621)
    Log::i("XR smoke resized: metres=",windowWidth,"x",physicalHeight);
  if(f==735) {
    Gothic::settingsSetI("INTERNAL","xrSurfaceLight",1);
    smokeCaptureUntil = frame.predictedDisplayTime+7'000'000'000;
    Log::i("XR smoke surface light: setting=",Gothic::settingsGetI("INTERNAL","xrSurfaceLight"));
    }
  if(smokeCaptureUntil>0 && frame.predictedDisplayTime>=smokeCaptureUntil) {
    smokeCaptureUntil = 0;
    Log::i("XR smoke complete");
    }
  hand.aimValid = false;
  hand.nearValid = false;
  hand.trigger = 0.f;
  hand.grab = 0.f;
  if(f>710)
    return;

  V3 point = buttonCenter();
  bool ray = false;
  if(f<320) {
    hand.grab = (f>=108 && f<116) || (f>=292 && f<300) ? 1.f : 0.f;
    } else if(f<530) {
    // Aim and trigger. Start at the light end and drag continuously.
    point.x += DimLeft+DimTravel*0.85f*std::clamp((float(f)-330.f)/94.f,0.f,1.f);
    ray = true;
    hand.trigger = f>=330 && f<500 ? 1.f : 0.f;
    } else {
    // Pinch the right corner near the screen, then extend from its original pose.
    const float t = std::clamp((float(f)-540.f)/50.f,0.f,1.f);
    point = {smokeWindow.x*0.5f+0.40f*t,-smokeWindow.y*0.5f-0.15f*t,0.01f};
    hand.grab = f>=540 && f<610 ? 1.f : 0.f;
    }
  XrPosef pose = windowPose;
  if(ray)
    point.z = 0.5f;
  pose.position = toXr(toWorld(windowPose,point));
  if(ray) {
    hand.aimValid = true;
    hand.aim = pose;
    } else {
    hand.nearValid = true;
    hand.nearPose = pose;
    }
  }

void XrWindow::Impl::updateInput() {
  releasePad = padState;
  padState = {};
  if(actionSet==XR_NULL_HANDLE || state!=XR_SESSION_STATE_FOCUSED || !AndroidApi::activityFocused())
    return;

  XrActiveActionSet active = {actionSet, XR_NULL_PATH};
  XrActionsSyncInfo sync   = {XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets      = &active;
  if(xrSyncActions(session, &sync)!=XR_SUCCESS) {
    cancelInput();
    return;
    }

  auto getF = [&](XrAction a, XrPath sub) -> std::optional<float> {
    if(a==XR_NULL_HANDLE || sub==XR_NULL_PATH)
      return std::nullopt;
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a; gi.subactionPath = sub;
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    if(xrGetActionStateFloat(session, &gi, &st)!=XR_SUCCESS || !st.isActive)
      return std::nullopt;
    return st.currentState;
    };
  auto getB = [&](XrAction a, XrPath sub) -> std::optional<bool> {
    if(a==XR_NULL_HANDLE || (a==aStickClick && sub==XR_NULL_PATH))
      return std::nullopt;
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a; gi.subactionPath = sub;
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    if(xrGetActionStateBoolean(session, &gi, &st)!=XR_SUCCESS || !st.isActive)
      return std::nullopt;
    return st.currentState!=XR_FALSE;
    };
  auto getV = [&](XrAction a, XrPath sub) -> std::optional<XrVector2f> {
    if(a==XR_NULL_HANDLE || sub==XR_NULL_PATH)
      return std::nullopt;
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a; gi.subactionPath = sub;
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    if(xrGetActionStateVector2f(session, &gi, &st)!=XR_SUCCESS || !st.isActive)
      return std::nullopt;
    return st.currentState;
    };

  const std::optional<XrVector2f> sticks[] = {getV(aStick,hands[0].path), getV(aStick,hands[1].path)};

  // Pointing at or touching the frame parts: the handle moves the window, the corners resize it.
  // The game image itself is never a grab target, so playing never moves the window by accident.
  const bool canManipulate = frame.shouldRender && placed && headValid;
  const V3 wNormal = rotate(windowPose.orientation, V3{0,0,1});
  auto rayHit = [&](const XrPosef& aim) -> std::optional<V3> {
    const V3 origin = toV3(aim.position);
    const V3 dir = rotate(aim.orientation,V3{0,0,-1});
    const float denom = dot(dir,wNormal);
    if(denom>=-1e-4f)
      return {};
    const float t = dot(toV3(windowPose.position)-origin,wNormal)/denom;
    if(t<=0.f)
      return {};
    return origin+dir*t;
    };

  for(size_t i=0; i<hands.size(); ++i) {
    auto& h = hands[i];
    auto locate = [&](XrSpace sp, XrPosef& out) {
      if(sp==XR_NULL_HANDLE)
        return false;
      XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
      const bool valid = XR_SUCCEEDED(xrLocateSpace(sp, localSpace, frame.predictedDisplayTime, &loc)) &&
                         (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                         (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
      out = loc.pose;
      return valid;
      };
    h.aimValid  = locate(h.aimSpace,  h.aim);
    h.nearValid = locate(h.nearSpace, h.nearPose);
    h.grab      = getF(aGrab, h.path);
    h.trigger   = getF(aTrigger,h.path);
    if(i==0 && CommandLine::inst().xrSmokeTest())
      smokeHand(h);
    h.hover     = Grip::None;
    h.hoverNear = false;

    // Direct touch first: the pinch point (or controller grip) close to a frame part
    if(h.nearValid) {
      const V3 n = toLocal(windowPose, toV3(h.nearPose.position));
      if(std::abs(n.z)<0.08f) {
        const Grip g = targetAt(n.x, n.y);
        if(g!=Grip::None) {
          h.hover     = g;
          h.hoverNear = true;
          h.hit       = toV3(h.nearPose.position);
          continue;
          }
        }
      }
    // Then pointing: the aim ray against the window plane
    if(!h.aimValid)
      continue;
    const auto p = rayHit(h.aim);
    if(!p)
      continue;
    const V3 l = toLocal(windowPose, *p);
    const Grip g = targetAt(l.x, l.y);
    if(g!=Grip::None) {
      h.hover = g;
      h.hit   = *p;
      }
    }

  for(size_t i=0; i<hands.size(); ++i) {
    auto& h = hands[i];
    // Track each physical control even when the other one owns the drag. An
    // inactive action or a held trigger on entry cannot synthesize a new press.
    auto freshPress = [](const std::optional<float>& action,bool& held) {
      const bool pressed = action && *action>0.75f;
      const bool fresh = pressed && !held;
      if(!action || pressed)
        held = true;
      else if(*action<0.35f)
        held = false;
      return fresh;
      };
    const bool gripDown = freshPress(h.grab,h.grabHeld);
    const bool triggerDown = freshPress(h.trigger,h.triggerHeld);
    const bool startTrigger = h.hover==Grip::Dim && triggerDown;
    auto moveDim = [&](const V3& worldPoint) {
      const float x = toLocal(windowPose,worldPoint).x;
      const float level = std::clamp((x-buttonCenter().x-DimLeft)/DimTravel,0.f,1.f);
      if(level!=passthroughDim && applyDim(level))
        dimDirty = true;
      };
    if(canManipulate && (h.hover==Grip::Dim || h.hover==Grip::Environment))
      lastDimHover = frame.predictedDisplayTime;
    if(grabbedBy==int(i)) {
      const bool tracked = grabNear ? h.nearValid : h.aimValid;
      const auto& action = grabTrigger ? h.trigger : h.grab;
      if(!action || *action<0.35f || !tracked ||
         ((grabbed==Grip::Environment || grabbed==Grip::Dim) && !roomAvailable()) ||
         (grabbed==Grip::Dim && !dimStyleAvailable)) {
        saveDim();
        grabbedBy = -1;
        grabbed = Grip::None;
        continue;
        }
      if(grabbed==Grip::Dim) {
        lastDimHover = frame.predictedDisplayTime;
        if(canManipulate) {
          if(grabNear)
            moveDim(toV3(h.nearPose.position));
          else if(auto hit = rayHit(h.aim))
            moveDim(*hit);
          }
        continue;
        }
      if(!canManipulate || grabbed==Grip::Environment) {
        lastGrabTime = frame.predictedDisplayTime;
        continue;
        }
      const XrPosef& holder = grabNear ? h.nearPose : h.aim;
      if(!grabNear) {
        // Thumbstick pushes a pointed-at window away or pulls it closer
        const XrVector2f stick = sticks[i].value_or(XrVector2f{});
        const float dt = std::clamp(float(frame.predictedDisplayTime-lastGrabTime)*1e-9f, 0.f, 0.05f);
        lastGrabTime = frame.predictedDisplayTime;
        // Preserve the original 72 Hz speed. A grab that starts outside the
        // travel limits can move back inside without snapping on contact.
        const float z = grabLocal.z;
        grabLocal.z = std::clamp(z-stick.y*2.16f*dt, std::min(z,-8.f), std::max(z,-0.2f));
        }
      const V3 point = toWorld(holder, grabLocal);
      h.hit   = point;
      h.hover = grabbed;

      if(grabbed==Grip::Handle) {
        // The handle stays in the hand; the window turns to face the viewer, without roll
        V3 center = point - rotate(windowPose.orientation, grabWindow);
        windowPose.orientation = facing(headPos - center);
        center = point - rotate(windowPose.orientation, grabWindow);
        windowPose.position = toXr(center);
        } else {
        // A corner follows the hand across the window plane; the centre stays put
        const V3    l     = toLocal(windowPose, point);
        const float dx = (l.x-grabWindow.x)*(grabWindow.x<0.f ? -2.f : 2.f);
        const float dy = (l.y-grabWindow.y)*(grabWindow.y<0.f ? -2.f : 2.f);
        windowWidth    = std::clamp(grabExtent.x+dx, 0.6f, 6.f);
        physicalHeight = std::clamp(grabExtent.y+dy, 0.3375f, 3.375f);
        }
      }
    else if(canManipulate && grabbedBy<0 && h.hover!=Grip::None && (gripDown || startTrigger)) {
      grabTrigger = startTrigger;
      grabbedBy = int(i);
      grabbed   = h.hover;
      grabNear  = h.hoverNear;
      lastGrabTime = frame.predictedDisplayTime;
      grabLocal = toLocal(grabNear ? h.nearPose : h.aim, h.hit);
      grabWindow = toLocal(windowPose, h.hit);
      if(grabbed==Grip::Dim) {
        moveDim(h.hit);
        } else if(grabbed==Grip::Environment) {
        if(setEnvironment(!immersive)) {
          restoreSky = false;
          Gothic::settingsSetI("INTERNAL", "xrEnvironment", immersive ? 1 : 0);
          Gothic::flushSettings();
          }
        } else if(grabbed!=Grip::Handle) {
        grabExtent = Tempest::Vec2(windowWidth,physicalHeight);
        }
      }
    }

  // Losing an action or taking the window interrupts gameplay. Only an active,
  // neutral sample rearms that control. Other sources can still release normally.
  const bool captured = grabbedBy>=0;
  auto admit = [&](size_t control, bool valid, bool held, bool wasHeld) {
    if(captured || !valid) {
      padInterrupted |= wasHeld;
      blockedControls[control] = !valid || held;
      return false;
      }
    if(!held)
      blockedControls.reset(control);
    return !blockedControls[control];
    };

  padState.connected = true;
  auto button = [&](PadState::Button id, XrAction action, XrPath path) {
    const auto value = getB(action,path);
    if(admit(id,value.has_value(),value.value_or(false),releasePad.buttons[id]))
      padState.buttons[id] = *value;
    else
      releasePad.buttons[id] = false;
    };
  button(PadState::A, aA, XR_NULL_PATH);
  button(PadState::B, aB, XR_NULL_PATH);
  button(PadState::X, aX, XR_NULL_PATH);
  button(PadState::Y, aY, XR_NULL_PATH);
  button(PadState::Start, aMenu, XR_NULL_PATH);
  button(PadState::LeftStick, aStickClick, hands[0].path);
  button(PadState::RightStick,aStickClick, hands[1].path);

  auto stick = [&](size_t id,const std::optional<XrVector2f>& value,float& x,float& y,float& oldX,float& oldY) {
    auto displaced = [](float a,float b){ return std::abs(a)>0.15f || std::abs(b)>0.15f; };
    if(admit(id,value.has_value(),value && displaced(value->x,value->y),displaced(oldX,oldY))) {
      x = value->x;
      y = -value->y;
      } else {
      oldX = oldY = 0;
      }
    };
  stick(StickLeft, sticks[0],padState.lx,padState.ly,releasePad.lx,releasePad.ly);
  stick(StickRight,sticks[1],padState.rx,padState.ry,releasePad.rx,releasePad.ry);

  auto trigger = [&](size_t id,const std::optional<float>& value,float& dst,float& old) {
    if(admit(id,value.has_value(),value.value_or(0.f)>0.35f,old>0.35f))
      dst = *value;
    else
      old = 0.f;
    };
  trigger(TriggerLeft, hands[0].trigger,padState.lt,releasePad.lt);
  trigger(TriggerRight,hands[1].trigger,padState.rt,releasePad.rt);
  }

V3 XrWindow::Impl::handleCenter() const {
  return V3{0, -windowHeight()*0.5f + edgeFeather()*0.5f - 0.05f, 0};
  }

Grip XrWindow::Impl::targetAt(float x, float y) const {
  const float halfW = windowWidth*0.5f;
  const float halfH = windowHeight()*0.5f;
  // Corners: a hand-sized zone around each bottom corner, mostly outside the image
  const float zone = 0.10f;
  const float inset = cornerInset();
  if(cornersReady && std::abs(y+halfH-inset)<zone) {
    if(std::abs(x-halfW+inset)<zone)
      return Grip::CornerRight;
    if(std::abs(x+halfW-inset)<zone)
      return Grip::CornerLeft;
    }
  const V3 hc = handleCenter();
  if(handleReady && std::abs(x-hc.x)<0.16f && std::abs(y-hc.y)<0.045f)
    return Grip::Handle;
  const V3 button = buttonCenter();
  if(buttonReady && roomAvailable() && std::abs(x-button.x)<0.065f && std::abs(y-button.y)<0.04f)
    return Grip::Environment;
  if(sliderRow()>=0 && x>=button.x+DimLeft-0.025f && x<=button.x+DimLeft+DimTravel+0.025f && std::abs(y-button.y)<0.032f)
    return Grip::Dim;
  return Grip::None;
  }

void XrWindow::Impl::locateEyes() {
  const bool scale = Gothic::settingsGetI("INTERNAL","xrResizeMode")==0;
  skipExtrapolation |= scale!=scalePicture;
  scalePicture = scale;
  XrViewLocateInfo info = {XR_TYPE_VIEW_LOCATE_INFO};
  info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  info.displayTime           = frame.predictedDisplayTime;
  info.space                 = localSpace;
  XrViewState st = {XR_TYPE_VIEW_STATE};
  XrView      views[2] = {{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
  uint32_t    count = 0;
  eyesValid = XR_SUCCEEDED(xrLocateViews(session, &info, &st, 2, &count, views)) && count==2 &&
              (st.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
  if(!eyesValid)
    return;
  const XrQuaternionf inv = conjugate(windowPose.orientation);
  for(int i=0; i<2; ++i)
    eyeInWindow[i] = rotate(inv, toV3(views[i].pose.position) - toV3(windowPose.position));

  // The same view as a projection layer: from the eye, facing the window plane, with an
  // asymmetric field of view that frames exactly the window rectangle (used with SpaceWarp)
  const float halfW = windowWidth*0.5f, halfH = windowHeight()*0.5f;
  for(int i=0; i<2; ++i) {
    V3 e = eyeInWindow[i];
    e.z  = std::max(e.z, 0.05f);
    eyePose[i].orientation = windowPose.orientation;
    eyePose[i].position    = toXr(toWorld(windowPose, e));
    eyeFov[i].angleLeft    = std::atan((-halfW - e.x)/e.z);
    eyeFov[i].angleRight   = std::atan(( halfW - e.x)/e.z);
    eyeFov[i].angleDown    = std::atan((-halfH - e.y)/e.z);
    eyeFov[i].angleUp      = std::atan(( halfH - e.y)/e.z);
    eyeNear[i]             = e.z;
    }
  }

bool XrWindow::eyeView(uint8_t eye, float verticalFovDeg, Camera::EyeView& out) const {
  auto& s = *impl;
  if(!s.eyesValid || eye>1)
    return false;
  // Scale-picture stretches the image in the window plane. Transform lateral eye motion by
  // the inverse magnification as well, so stereo still converges at the physical window.
  // Eye distance and depth stay in metres in both modes, including for SpaceWarp.
  const float units = 100.f;
  const float unitsX = s.scalePicture ? units*(2.4f/s.windowWidth) : units;
  const float unitsY = s.scalePicture ? units*(1.35f/s.windowHeight()) : units;
  const float halfW = s.windowWidth*0.5f*unitsX;
  const float halfH = s.windowHeight()*0.5f*unitsY;
  const float plane = Impl::portalDistance(verticalFovDeg)*units;

  V3 e = s.eyeInWindow[eye];
  e.z  = std::max(e.z, 0.05f);   // behind or inside the window: hold the eye just in front of it
  // View space of the game camera: +x right, +y down (Vulkan), +z forward
  const float ex = e.x*unitsX;
  const float ey = -e.y*unitsY;
  const float dz = e.z*units;    // eye distance to the window plane
  out.offset = Tempest::Vec3(ex, ey, plane-dz);
  out.zNear  = dz;
  out.left   = -halfW - ex;
  out.right  =  halfW - ex;
  out.bottom = -halfH - ey;
  out.top    =  halfH - ey;
  return true;
  }

bool XrWindow::isWindowFilterSupported() const {
  return impl->hasWindowFilter;
  }

bool XrWindow::isFoveationSupported() const {
  return impl->rateTexel.width!=0;
  }

void XrWindow::prepareLightingRate(Encoder<CommandBuffer>& cmd, uint8_t slot, uint8_t eye, uint32_t w, uint32_t h) {
  auto& s = *impl;
  if(!isFoveationSupported() || !s.placed || !s.headValid || !s.eyesValid || eye>=2 || slot>=Resources::MaxFramesInFlight)
    return;
  const auto e = s.eyeInWindow[eye];
  if(!(e.z>0.f) || w==0 || h==0 || s.frame.predictedDisplayTime<s.retryRateAfter)
    return;
  const auto f = normalize(rotate(conjugate(s.windowPose.orientation),s.headForward));
  auto& map = s.lightingRate[slot];
  if(!map.prepare(s.vk,s.rateTexel,w,h,{s.windowWidth,s.windowHeight()},{e.x,e.y,e.z},{f.x,f.y,f.z})) {
    // A transient allocation or flush failure should take both eyes back to full rate.
    s.retryRateAfter = s.frame.predictedDisplayTime + 3'000'000'000LL;
    Log::e("OpenXR: lighting rate map unavailable, retrying in three seconds");
    return;
    }
  vulkanSetNextShadingRate(cmd,map.rate);
  }

bool XrWindow::pollInputFocus(bool nativeFocusLost) {
  const bool interrupted = impl->pollEvents();
  if(nativeFocusLost || !AndroidApi::activityFocused())
    impl->cancelInput();
  return !interrupted && isFocused();
  }

void XrWindow::Impl::invalidateSurface() {
  if(!probePending) {
    environmentDepth.cancel();
    surfaceLight.invalidate();
    probePending = true;
    }
  }

void XrWindow::Impl::updateSurface() {
  surfaceCanProbe = false;
  const int setting = Gothic::settingsGetI("INTERNAL","xrSurfaceLight");
  if(traceSurface && tracedSurfaceSetting!=setting) {
    tracedSurfaceSetting=setting;
    Log::i("XR surface setting=",setting," depthSupported=",int(hasDepth)," additiveBlend=",int(hasAlphaBlend));
    }
  const bool enabled = hasDepth && setting!=0;
  if(!enabled) {
    if(lightEnabled) {
      invalidateSurface();
      permissionAsked = permissionGranted = permissionHintShown = false;
      nextPermissionCheck = 0;
      }
    lightEnabled = false;
    return;
    }
  lightEnabled = true;
  const bool moving = grabbed==Grip::Handle || grabbed==Grip::CornerLeft || grabbed==Grip::CornerRight;
  const bool focused = state==XR_SESSION_STATE_FOCUSED && AndroidApi::activityFocused();
  if(traceSurface) {
    const unsigned gate = unsigned(placed) | (unsigned(headValid)<<1) | (unsigned(moving)<<2) | (unsigned(focused)<<3);
    if(gate!=tracedProbeGate) {
      tracedProbeGate=gate;
      Log::i("XR surface probe gate placed=",int(placed)," tracking=",int(headValid)," moving=",int(moving),
             " focused=",int(focused));
      }
    }
  if(!placed || !headValid || moving || !focused || !frame.shouldRender) {
    invalidateSurface();
    return;
    }
  if(!permissionGranted) {
    if(frame.predictedDisplayTime<nextPermissionCheck)
      return;
    permissionGranted = xrSpatialPermission(!permissionAsked);
    if(traceSurface)
      Log::i("XR surface spatial permission=",permissionGranted ? "granted" : "missing",
             " request=",int(!permissionAsked && !permissionGranted));
    if(!permissionGranted && permissionAsked && !permissionHintShown) {
      Gothic::inst().onPrint("Surface light needs Spatial Data permission. Allow it in the app permissions.");
      permissionHintShown=true;
      }
    permissionAsked = true;
    nextPermissionCheck = frame.predictedDisplayTime+2000000000;
    if(!permissionGranted)
      return;
    }
  surfaceCanProbe = true;
  }

bool XrWindow::isSurfaceLightSupported() const {
  return impl->hasDepth;
  }

void XrWindow::drawSurfaceLight(Renderer& renderer, Encoder<CommandBuffer>& cmd, bool sceneReady) {
  auto& s = *impl;
  if(!s.surfaceCanProbe)
    return;
  if(!sceneReady) {
    s.invalidateSurface();
    return;
    }
  // Like the Meta occlusion sample, acquire depth on a rendering frame after
  // locating the eye views and successfully acquiring/waiting the colour image.
  // This entry point runs exactly once, for the first eye.
  if(s.probePending) {
    s.probePending = false;
    s.depthReady = s.environmentDepth.initialize(s.instance,s.system,s.session,*s.device);
    if(s.traceSurface) Log::i("XR surface depth initialized=",int(s.depthReady));
    if(s.depthReady)
      s.environmentDepth.request(s.windowPose,{s.windowWidth,s.windowHeight()});
    }
  if(s.depthReady)
    s.environmentDepth.update(s.localSpace,s.frame.predictedDisplayTime);
  if(!s.environmentDepth.surface())
    return;
  const auto& plane = *s.environmentDepth.surface();
  if(auto* target = s.surfaceLight.target(*s.device,s.session,s.eyeColorFormat,*s.vk.queueLock,s.frame.predictedDisplayTime)) {
    const bool written = renderer.drawSurfaceLight(cmd,*target,{plane.uvOffset.x,plane.uvOffset.y},
                            {plane.uvScale.x,plane.uvScale.y},s.passthroughDim,plane.drop,s.eyeColorFormat==VK_FORMAT_R8G8B8A8_SRGB);
    s.surfaceLight.recorded(written);
    if(s.traceSurface) Log::i("XR surface texture recorded=",int(written));
    }
  }

int64_t XrWindow::nominalFramePeriod() const {
  if(!(impl->refreshRate>=30.f && impl->refreshRate<=240.f))
    return 0;
  return int64_t(std::llround((impl->spaceWarpWanted ? 2e9 : 1e9)/double(impl->refreshRate)));
  }

int64_t XrWindow::predictedDisplayTime() const {
  return impl->frame.predictedDisplayTime;
  }

bool XrWindow::beginFrame() {
  auto& s = *impl;
  if(!s.running || s.state==XR_SESSION_STATE_STOPPING) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return false;
    }

  XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
  s.frame = {XR_TYPE_FRAME_STATE};
  if(!s.sessionResult(xrWaitFrame(s.session, &wait, &s.frame), "xrWaitFrame"))
    return false;
  {
  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
  if(!s.sessionResult(xrBeginFrame(s.session, &begin), "xrBeginFrame"))
    return false;
  }
  s.frameBegun = true;

  if(isVisible()) {
    const int style = Gothic::settingsGetI("INTERNAL","xrWindowEdges");
    const int nextStyle = (style==1 || style==2) ? style : 0;
    const bool contourChanged = s.edgeStyle!=nextStyle;
    s.edgeStyle = nextStyle;
    s.placeWindow();
    // No motion vector can describe an instantaneous change in source coverage.
    s.skipExtrapolation |= contourChanged;
    // A focused game keeps ticking even when the runtime requests zero layers.
    s.updateInput();
    s.updateSurface();
    if(s.frame.shouldRender) {
      s.locateEyes();
      return true;
      }
    }
  submit(false);
  return false;
  }

void XrWindow::drawWindowEdges(Encoder<CommandBuffer>& cmd, bool motion) {
  auto& s = *impl;
  if(s.edgeStyle==0)
    return;
  if(s.edgeCoverage.isEmpty()) {
    auto shader = [&](const char* name) {
      const auto code = GothicShader::get(name);
      return s.device->shader(code.data,code.len);
      };
    RenderState state;
    state.setZWriteEnabled(false);
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    state.setBlendSource(RenderState::BlendMode::OneMinusDstColor);
    state.setBlendDest(RenderState::BlendMode::One);
    s.edgeOpaque = s.device->pipeline(Triangles,state,shader("triangle.vert.sprv"),shader("window_opaque.frag.sprv"));
    state.setBlendSource(RenderState::BlendMode::Zero);
    state.setBlendDest(RenderState::BlendMode::SrcColor);
    auto border = shader("window_edge.vert.sprv");
    s.edgeCoverage = s.device->pipeline(Triangles,state,border,shader("window_edge.frag.sprv"));
    state.setBlendSource(RenderState::BlendMode::One);
    state.setBlendDest(RenderState::BlendMode::Zero);
    state.setZWriteEnabled(true);
    s.edgeMotion = s.device->pipeline(Triangles,state,border,shader("window_edge_motion.frag.sprv"));
    }
  // Both draws stay in the active eye colour pass. With source (0,0,0,1)
  // the first blend gives RGB = dst.rgb and A = 1 - dst.a + dst.a.
  cmd.setDebugMarker(motion ? "XR window edge motion" : "XR window edges");
  if(!motion) {
    cmd.setPipeline(s.edgeOpaque);
    cmd.draw(nullptr,0,3);
    }
  const auto w = motion ? s.mvWidth : s.width;
  const auto h = motion ? s.mvHeight : s.height;
  struct Push {
    Vec2 size, metresPerPixel;
    float radius, feather;
    } push{{s.windowWidth,s.windowHeight()},{s.windowWidth/float(w),s.windowHeight()/float(h)},
           s.edgeRadius(),s.edgeFeather()};
  cmd.setPipeline(motion ? s.edgeMotion : s.edgeCoverage);
  cmd.setPushData(push);
  cmd.draw(nullptr,0,6,0,4);
  }

// Store straight RGB so the compositor applies coverage after decoding the sRGB swapchain.
static uint32_t pixel(float r, float g, float b, float o, float a) {
  a = std::clamp(a, 0.f, 1.f)*o;
  auto c = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f)*255.f + 0.5f); };
  return c(r) | (c(g)<<8) | (c(b)<<16) | (c(a)<<24);
  }

static bool uploadStatic(const VulkanNativeContext& vk, VkCommandBuffer cb, VkFence fence, XrSwapchain sc,
                         uint32_t w, uint32_t h, uint32_t layers, const std::vector<uint32_t>& rgba) {
  const auto img = swapchainImages(sc);
  if(img.empty())
    return false;
  // Host-visible staging buffer
  const VkDeviceSize size = VkDeviceSize(rgba.size()*sizeof(uint32_t));
  VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size  = size;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  struct Staging {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~Staging() {
      vkDestroyBuffer(device, buffer, nullptr);
      vkFreeMemory(device, memory, nullptr);
      }
    } staging{vk.device};
  auto& buf = staging.buffer;
  auto& mem = staging.memory;
  if(vkCreateBuffer(vk.device, &bi, nullptr, &buf)!=VK_SUCCESS)
    return false;
  VkMemoryRequirements req = {};
  vkGetBufferMemoryRequirements(vk.device, buf, &req);
  VkPhysicalDeviceMemoryProperties mp = {};
  vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice, &mp);
  uint32_t type = mp.memoryTypeCount;
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for(uint32_t i=0; i<mp.memoryTypeCount; ++i)
    if((req.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & want)==want) {
      type = i;
      break;
      }
  if(type==mp.memoryTypeCount)
    return false;
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize  = req.size;
  ai.memoryTypeIndex = type;
  if(vkAllocateMemory(vk.device, &ai, nullptr, &mem)!=VK_SUCCESS ||
     vkBindBufferMemory(vk.device, buf, mem, 0)!=VK_SUCCESS)
    return false;
  void* data = nullptr;
  if(vkMapMemory(vk.device, mem, 0, size, 0, &data)!=VK_SUCCESS)
    return false;
  std::memcpy(data, rgba.data(), size_t(size));
  vkUnmapMemory(vk.device, mem);

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if(!ok(xrAcquireSwapchainImage(sc, &acq, &index), "acquire static image") ||
     !ok(xrWaitSwapchainImage(sc, &wait), "wait for static image"))
    return false;

  // Each upload waits for its own submission before freeing the staging memory.
  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if(vkResetCommandBuffer(cb, 0)!=VK_SUCCESS || vkBeginCommandBuffer(cb, &cbi)!=VK_SUCCESS)
    return false;
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = img[index].image;
  b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  for(uint32_t l=0; l<layers; ++l) {
    VkBufferImageCopy r = {};
    r.bufferOffset     = VkDeviceSize(l)*w*h*4;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1};
    r.imageExtent      = {w, h, 1};
    vkCmdCopyBufferToImage(cb, buf, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
  b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = 0;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  if(vkEndCommandBuffer(cb)!=VK_SUCCESS)
    return false;
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb;
  if(vkResetFences(vk.device, 1, &fence)!=VK_SUCCESS)
    return false;
  const VkResult submitted = vkQueueSubmit(vk.queue, 1, &si, fence);
  if(submitted!=VK_SUCCESS && submitted!=VK_ERROR_DEVICE_LOST)
    return false;

  // A wait can run out of memory even after the copy was accepted. Its staging
  // buffer and command pool must survive until completion or terminal device loss.
  VkResult waited = vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX);
  if(waited==VK_ERROR_OUT_OF_HOST_MEMORY || waited==VK_ERROR_OUT_OF_DEVICE_MEMORY) {
    do {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      waited = vkGetFenceStatus(vk.device,fence);
      } while(waited==VK_NOT_READY || waited==VK_ERROR_OUT_OF_HOST_MEMORY || waited==VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
  if(waited==VK_ERROR_DEVICE_LOST || submitted==VK_ERROR_DEVICE_LOST)
    SystemApi::exit();
  if(waited!=VK_SUCCESS || submitted!=VK_SUCCESS)
    return false;

  XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  return ok(xrReleaseSwapchainImage(sc, &rel), "release static image");
  }

// Equirectangular sRGB panorama. All frequencies around the horizon are
// integral, so the mountain profiles meet exactly at the longitude seam.
static std::vector<uint32_t> duskPanorama(uint32_t w, uint32_t h) {
  std::vector<uint32_t> pixels(size_t(w)*h);
  auto rgb = [](float r, float g, float b) {
    auto byte = [](float v) { return uint32_t(std::clamp(v,0.f,255.f)+0.5f); };
    return byte(r) | byte(g)<<8 | byte(b)<<16 | 0xff000000u;
    };
  const float pi = float(M_PI);
  for(uint32_t x=0; x<w; ++x) {
    const float longitude = (float(x)+0.5f)*2.f*pi/float(w);
    const float glow = std::pow(0.5f+0.5f*std::cos(longitude-1.7f), 3.f);
    const float farRidge = 0.067f + 0.035f*std::sin(3*longitude) + 0.020f*std::sin(9*longitude+1.3f)
                           + 0.013f*std::cos(16*longitude) + 0.003f*std::sin(57*longitude);
    const float nearRidge = 0.017f + 0.021f*std::cos(4*longitude+1.f) + 0.009f*std::sin(11*longitude)
                            + 0.003f*std::sin(41*longitude+2.f);
    for(uint32_t y=0; y<h; ++y) {
      const float lat = (0.5f-(float(y)+0.5f)/float(h))*pi;
      const float haze = std::exp(-std::abs(lat)*3.7f);
      float r = 13.f+(47.f+37.f*glow)*haze;
      float g = 20.f+(42.f+12.f*glow)*haze;
      float b = 39.f+40.f*haze;
      if(lat<farRidge) {
        r = 37.f+8.f*glow;
        g = 43.f+3.f*glow;
        b = 59.f;
        }
      if(lat<nearRidge) {
        r = 24.f;
        g = 30.f;
        b = 42.f;
        }
      if(lat<-0.037f) {
        const float ground = std::exp((lat+0.037f)*6.f);
        r = 15.f + 10.f*ground;
        g = 20.f + 12.f*ground;
        b = 28.f + 16.f*ground;
        }
      // Ordered sub-byte noise breaks up colour bands in the dark gradients.
      const float grain = float((x*13u+y*7u+(x^y)*3u)%17u)/17.f-0.5f;
      pixels[size_t(y)*w+x] = rgb(r+grain, g+grain, b+grain);
      }
    }
  uint32_t seed = 0x38a41b7du;
  auto sample = [&]() {
    seed = seed*1664525u+1013904223u;
    return float(seed>>8)*(1.f/16777216.f);
    };
  for(unsigned i=0; i<1200; ++i) {
    const uint32_t x = uint32_t(sample()*float(w));
    const float latitude = std::asin(0.17f+0.82f*sample());
    const uint32_t y = uint32_t((0.5f-latitude/pi)*float(h));
    const float shine = 68.f + 92.f*std::pow(sample(), 4.f);
    pixels[size_t(y)*w+x] = rgb(shine*0.89f, shine*0.94f, shine);
    }
  return pixels;
  }

bool XrWindow::Impl::setEnvironment(bool useSky) {
  if(useSky && !panoramaReady)
    return false;
  if(immersive==useSky)
    return true;
  if(passthrough!=XR_NULL_HANDLE &&
     !ok(useSky ? xrPausePassthrough(passthrough) : xrStartPassthrough(passthrough), "switch passthrough"))
    return false;
  immersive = useSky;
  lastDimHover = 0;
  Log::i("OpenXR: environment ", immersive ? "dusk panorama" : "passthrough");
  return true;
  }

void XrWindow::Impl::fillStaticImages() {
  if(handleSwapchain==XR_NULL_HANDLE && cornerSwapchain==XR_NULL_HANDLE && panorama==XR_NULL_HANDLE)
    return;

  auto available = [](VkResult result) {
    if(result==VK_SUCCESS)
      return true;
    Log::e("OpenXR: optional artwork upload setup failed, Vulkan result ", int(result));
    if(result!=VK_ERROR_OUT_OF_HOST_MEMORY && result!=VK_ERROR_OUT_OF_DEVICE_MEMORY)
      SystemApi::exit();
    return false;
    };

  VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = vk.queueFamily;
  VkCommandPool pool = VK_NULL_HANDLE;
  if(!available(vkCreateCommandPool(vk.device, &pi, nullptr, &pool)))
    return;
  struct UploadResources {
    VkDevice device;
    VkCommandPool pool;
    VkFence fence = VK_NULL_HANDLE;
    ~UploadResources() {
      vkDestroyFence(device, fence, nullptr);
      vkDestroyCommandPool(device, pool, nullptr);
      }
    } resources{vk.device, pool};

  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if(!available(vkAllocateCommandBuffers(vk.device, &alloc, &cmd)))
    return;
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  if(!available(vkCreateFence(vk.device, &fi, nullptr, &fence)))
    return;
  resources.fence = fence;

  // All artwork uploads are synchronous and share these resources for this frame only.
  for(auto fill : {&Impl::fillHandle, &Impl::fillCorners, &Impl::fillEnvironment, &Impl::fillSlider}) {
    try {
      (this->*fill)(cmd,fence);
      }
    catch(const std::bad_alloc&) {
      Log::e("OpenXR: insufficient memory for optional window graphics");
      }
    }
  }

void XrWindow::Impl::fillEnvironment(VkCommandBuffer cmd, VkFence fence) {
  if(panorama==XR_NULL_HANDLE)
    return;
  panoramaReady = uploadStatic(vk, cmd, fence, panorama, SkyW, SkyH, 1, duskPanorama(SkyW,SkyH));
  if(!panoramaReady)
    return;
  if(environmentButton!=XR_NULL_HANDLE && hasPassthrough) {
    std::vector<uint32_t> px(size_t(ButtonW)*ButtonH*4);
    for(unsigned layer=0; layer<4; ++layer) {
      const bool sky = layer>=2, hover = (layer&1)!=0;
      for(uint32_t y=0; y<ButtonH; ++y)
        for(uint32_t x=0; x<ButtonW; ++x) {
          const float u = float(x)+0.5f-float(ButtonW)*0.5f;
          const float v = float(y)+0.5f-float(ButtonH)*0.5f;
          const float rim = std::hypot(std::max(std::abs(u)-22.f,0.f), v);
          // A crescent invites entering VR. A window icon returns to the room.
          const float moon = std::min(16.f-std::hypot(u,v), std::hypot(u-8.f,v+5.f)-14.f);
          const float window = 2.f-std::abs(std::max(std::abs(u)-17.f, std::abs(v)-12.f));
          const float ink = std::clamp((sky ? window : moon)+0.5f,0.f,1.f);
          const float edge = std::clamp(1.5f-std::abs(rim-32.f),0.f,1.f);
          const float light = std::max(ink,edge*0.62f);
          px[(size_t(layer)*ButtonH+y)*ButtonW+x] =
              pixel(0.12f+0.84f*light,0.15f+0.82f*light,0.20f+0.8f*light,
                    hover ? 0.97f : 0.58f+0.27f*light, 34.f-rim);
          }
      }
    buttonReady = uploadStatic(vk, cmd, fence, environmentButton, ButtonW, ButtonH, 4, px);
    }
  // Settings exist by the first rendered frame. An incomplete button should
  // never strand a wearer in VR when passthrough is available.
  if(buttonReady && Gothic::settingsGetI("INTERNAL","xrEnvironment")==1)
    restoreSky = !setEnvironment(true);
  }

void XrWindow::Impl::fillSlider(VkCommandBuffer cmd, VkFence fence) {
  // Settings become available on the first rendered frame. A replacement
  // passthrough layer receives the same style from createPassthrough().
  const float saved = Gothic::settingsGetF("INTERNAL","xrPassthroughDim");
  passthroughDim = std::isfinite(saved) ? std::clamp(saved,0.f,1.f) : 0.f;
  if(!buttonReady || dimSlider==XR_NULL_HANDLE)
    return;

  constexpr int width = RailW+KnobW, height = SliderRows*KnobW;
  std::vector<uint32_t> px(size_t(width)*height);
  for(int y=0; y<height; ++y) {
    const float fade = float(y/KnobW+1)/float(SliderRows);
    const float dy = float(y%KnobW)+0.5f-float(KnobW)/2.f;
    for(int x=0; x<width; ++x) {
      const bool knob = x>=RailW;
      const float dx = knob ? float(x-RailW)+0.5f-float(KnobW)*0.5f :
                              std::max(std::abs(float(x)+0.5f-float(RailW)*0.5f)-73.f,0.f);
      const float radius = knob ? 9.f : 5.f;
      const float d = std::hypot(dx,dy);
      const float ink = knob ? std::clamp(radius-2.f-d,0.f,1.f) :
                               std::clamp(1.35f-std::abs(d-radius+1.f),0.f,1.f)*0.45f;
      px[size_t(y)*width+size_t(x)] = pixel(0.12f+0.83f*ink,0.15f+0.82f*ink,0.2f+0.8f*ink,
                                    fade*(knob ? 0.96f : 0.82f),radius-d+0.5f);
      }
    }
  sliderReady = uploadStatic(vk,cmd,fence,dimSlider,width,height,1,px);
  if(sliderReady && passthroughDim>0.f)
    applyDim(passthroughDim);
  }

void XrWindow::Impl::fillHandle(VkCommandBuffer cmd, VkFence fence) {
  // Handle: a capsule; idle, hover and held states
  if(handleSwapchain!=XR_NULL_HANDLE) {
    const uint32_t w = HandleW, h = HandleH;
    std::vector<uint32_t> px(size_t(w)*h*3);
    const float r   = float(h)*0.5f - 2.f;
    const float len = float(w)*0.5f - float(h)*0.5f;
    const struct { float r,g,b,o; } st[3] = {{0.9f,0.9f,0.9f,0.45f}, {1.f,1.f,1.f,0.9f}, {0.8f,0.9f,1.f,1.f}};
    for(uint32_t l=0; l<3; ++l)
      for(uint32_t y=0; y<h; ++y)
        for(uint32_t x=0; x<w; ++x) {
          const float fx = float(x)+0.5f - float(w)*0.5f;
          const float fy = float(y)+0.5f - float(h)*0.5f;
          const float dx = std::max(std::abs(fx)-len, 0.f);
          const float d  = std::sqrt(dx*dx + fy*fy);
          px[(size_t(l)*h + y)*w + x] = pixel(st[l].r, st[l].g, st[l].b, st[l].o, r - d + 0.5f);
          }
    handleReady = uploadStatic(vk, cmd, fence, handleSwapchain, w, h, 3, px);
    }
  }

void XrWindow::Impl::fillCorners(VkCommandBuffer cmd, VkFence fence) {
  // Corners: an L bracket around the bottom-right corner (layers 0,1) and its mirror (2,3)
  if(cornerSwapchain!=XR_NULL_HANDLE) {
    const uint32_t n = CornerSize;
    std::vector<uint32_t> px(size_t(n)*n*4);
    const float t = 3.5f;                 // half thickness
    const float c = 44.f;                 // bracket corner, pixels
    const float a = 10.f;                 // arm ends
    const struct { float r,g,b,o; } st[2] = {{1.f,1.f,1.f,0.9f}, {0.8f,0.9f,1.f,1.f}};
    for(uint32_t l=0; l<4; ++l)
      for(uint32_t y=0; y<n; ++y)
        for(uint32_t x=0; x<n; ++x) {
          float fx = float(x)+0.5f;
          float fy = float(y)+0.5f;
          if(l>=2)
            fx = float(n)-fx;
          // distance to the two segments (a,c)-(c,c) and (c,a)-(c,c)
          const float hx = std::clamp(fx, a, c), vy = std::clamp(fy, a, c);
          const float d1 = std::sqrt((fx-hx)*(fx-hx) + (fy-c)*(fy-c));
          const float d2 = std::sqrt((fx-c)*(fx-c) + (fy-vy)*(fy-vy));
          const auto& s = st[l%2];
          px[(size_t(l)*n + y)*n + x] = pixel(s.r, s.g, s.b, s.o, t - std::min(d1,d2) + 0.5f);
          }
    cornersReady = uploadStatic(vk, cmd, fence, cornerSwapchain, n, n, 4, px);
    }
  }

Attachment* XrWindow::eyeTarget(uint8_t eye) {
  auto& s = *impl;
  if(eye>1 || s.targets[eye].empty())
    return nullptr;
  if(s.acquired[eye]<0) {
    std::lock_guard<std::mutex> guard(*s.vk.queueLock);
    uint32_t index = 0;
    const uint64_t t0 = xrUs();
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(!ok(xrAcquireSwapchainImage(s.swapchain[eye], &acq, &index), "acquire window image"))
      return nullptr;
    const uint64_t t1 = xrUs();
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if(xrWaitSwapchainImage(s.swapchain[eye], &wait)!=XR_SUCCESS) {
      Log::e("OpenXR: failed to wait for acquired image");
      SystemApi::exit();
      return nullptr;
      }
    tAcquire  += t1-t0;
    tSwapWait += xrUs()-t1;
    s.acquired[eye] = int32_t(index);
    }
  auto& target = s.targets[eye][size_t(s.acquired[eye])];
  vulkanSetBorrowedExtent(target,s.width,s.height);
  return &target;
  }

bool XrWindow::isSpaceWarpSupported() const {
  return impl->hasSpaceWarp;
  }

bool XrWindow::prepareSpaceWarp(bool use, bool previousViewValid) {
  impl->spaceWarpWanted = use && impl->hasSpaceWarp;
  impl->skipExtrapolation |= !previousViewValid;
  return impl->spaceWarpWanted && !impl->skipExtrapolation;
  }

Attachment* XrWindow::motionTarget(uint8_t eye) {
  auto& s = *impl;
  if(!s.spaceWarpWanted || eye>1 || s.mvTargets[eye].empty())
    return nullptr;
  if(s.mvAcquired[eye]<0) {
    std::lock_guard<std::mutex> guard(*s.vk.queueLock);
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(!ok(xrAcquireSwapchainImage(s.mvSwapchain[eye], &acq, &index), "acquire motion vectors"))
      return nullptr;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if(xrWaitSwapchainImage(s.mvSwapchain[eye], &wait)!=XR_SUCCESS) {
      Log::e("OpenXR: failed to wait for acquired image");
      SystemApi::exit();
      return nullptr;
      }
    s.mvAcquired[eye] = int32_t(index);
    }
  auto& target = s.mvTargets[eye][size_t(s.mvAcquired[eye])];
  vulkanSetBorrowedExtent(target,s.mvWidth,s.mvHeight);
  return &target;
  }

ZBuffer* XrWindow::depthTarget(uint8_t eye) {
  auto& s = *impl;
  if(!s.spaceWarpWanted || eye>1 || s.depthTargets[eye].empty())
    return nullptr;
  if(s.depthAcquired[eye]<0) {
    std::lock_guard<std::mutex> guard(*s.vk.queueLock);
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(!ok(xrAcquireSwapchainImage(s.depthSwapchain[eye], &acq, &index), "acquire depth"))
      return nullptr;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if(xrWaitSwapchainImage(s.depthSwapchain[eye], &wait)!=XR_SUCCESS) {
      Log::e("OpenXR: failed to wait for acquired image");
      SystemApi::exit();
      return nullptr;
      }
    s.depthAcquired[eye] = int32_t(index);
    }
  auto& target = s.depthTargets[eye][size_t(s.depthAcquired[eye])];
  vulkanSetBorrowedExtent(target,s.mvWidth,s.mvHeight);
  return &target;
  }

bool XrWindow::Impl::releaseEyes() {
  const uint64_t t0 = xrUs();
  bool released = true;
  for(int eye=0; eye<2; ++eye)
    released &= releaseImage(swapchain[eye], acquired[eye]);
  tRelease += xrUs()-t0;
  return released;
  }

bool XrWindow::submit(bool rendered) {
  auto& s = *impl;
  if(!s.frameBegun)
    return false;
  s.frameBegun = false;

  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  if(rendered && !s.staticImagesAttempted) {
    s.staticImagesAttempted = true;
    s.fillStaticImages();
    }
  // A LOCAL recenter invalidates the old pose until the next valid placement.
  bool hasImage = rendered && s.placed && s.acquired[0]>=0 && s.acquired[1]>=0;
  bool warp = hasImage && s.spaceWarpWanted &&
                        s.mvAcquired[0]>=0 && s.mvAcquired[1]>=0 && s.depthAcquired[0]>=0 && s.depthAcquired[1]>=0;
  s.surfaceLight.finish(hasImage);
  const bool eyesReleased = s.releaseEyes();
  const bool warpReleased = s.releaseSpaceWarp();
  if(!eyesReleased || !warpReleased) {
    hasImage = warp = false;
    SystemApi::exit();
    }

  // Background, eyes, button, slider rail and thumb, handle and corner brackets.
  std::array<const XrCompositionLayerBaseHeader*,10> layers;
  uint32_t layerCount = 0;
  auto addLayer = [&](const auto& layer) {
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    };

  XrCompositionLayerPassthroughFB pt = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  XrCompositionLayerEquirect2KHR sky = {XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
  XrCompositionLayerEquirectKHR sky1 = {XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR};
  if(s.panoramaReady && (s.immersive || !s.roomAvailable())) {
    sky.space = s.localSpace;
    sky.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    sky.subImage.swapchain = s.panorama;
    sky.subImage.imageRect.extent = {int32_t(Impl::SkyW), int32_t(Impl::SkyH)};
    sky.pose.orientation.w = 1.f;
    sky.radius = 0.f; // infinite sphere, stable under head translation
    sky.centralHorizontalAngle = 2.f*float(M_PI);
    sky.upperVerticalAngle = float(M_PI)*0.5f;
    sky.lowerVerticalAngle = -sky.upperVerticalAngle;
    if(s.hasEquirect2) {
      addLayer(sky);
      } else {
      sky1.space = sky.space;
      sky1.eyeVisibility = sky.eyeVisibility;
      sky1.subImage = sky.subImage;
      sky1.pose = sky.pose;
      sky1.scale = {1.f,1.f};
      addLayer(sky1);
      }
    } else if(s.roomAvailable()) {
    pt.layerHandle = s.passthroughLayer;
    if(s.dimUsesOpacity && s.passthroughDim>0.f)
      pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    addLayer(pt);
    }

  const XrCompositionLayerQuad* spill = nullptr;
  if(hasImage && s.lightEnabled && s.environmentDepth.surface()) {
    const auto& surface = *s.environmentDepth.surface();
    spill = s.surfaceLight.layer(s.localSpace,surface.pose,surface.size,s.hasAlphaBlend);
    if(spill)
      addLayer(*spill);
    }
  // The same rectangle in the room for both eyes, each showing its own view through it
  XrCompositionLayerQuad quad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},{XR_TYPE_COMPOSITION_LAYER_QUAD}};
  // SpaceWarp: the window as a projection layer with motion vectors and depth
  XrCompositionLayerProjection      proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  if(s.edgeStyle!=0) {
    // The sRGB swapchain stores straight colour. Premultiply after the compositor's sRGB decode.
    proj.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
    for(auto& q:quad)
      q.layerFlags = proj.layerFlags;
    }
  XrCompositionLayerSettingsFB filter = {XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
  if(hasImage && s.hasWindowFilter && Gothic::settingsGetI("INTERNAL","xrWindowFilter")==1) {
    filter.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB |
                        XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB;
    proj.next = &filter;
    for(auto& q:quad)
      q.next = &filter;
    }
  XrCompositionLayerProjectionView  pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerSpaceWarpInfoFB sw[2] = {{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}};
  if(warp) {
    for(int eye=0; eye<2; ++eye) {
      auto& w = sw[eye];
      w.layerFlags = s.skipExtrapolation ? XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB : 0;
      w.motionVectorSubImage.swapchain       = s.mvSwapchain[eye];
      w.motionVectorSubImage.imageRect       = {{0,0},{int32_t(s.mvWidth),int32_t(s.mvHeight)}};
      w.depthSubImage.swapchain              = s.depthSwapchain[eye];
      w.depthSubImage.imageRect              = {{0,0},{int32_t(s.mvWidth),int32_t(s.mvHeight)}};
      w.appSpaceDeltaPose                    = {{0,0,0,1},{0,0,0}};
      w.minDepth                             = 0.f;
      w.maxDepth                             = 1.f;
      // Life-size portal: game depth in cm is real depth from the eye
      w.nearZ                                = s.eyeNear[eye];
      w.farZ                                 = 1000.f;

      auto& v = pv[eye];
      v.next                     = &w;
      v.pose                     = s.eyePose[eye];
      v.fov                      = s.eyeFov[eye];
      v.subImage.swapchain       = s.swapchain[eye];
      v.subImage.imageRect       = {{0,0},{int32_t(s.width),int32_t(s.height)}};
      v.subImage.imageArrayIndex = 0;
      }
    proj.space     = s.localSpace;
    proj.viewCount = 2;
    proj.views     = pv;
    addLayer(proj);
    }

  if(hasImage && !warp) {
    for(int eye=0; eye<2; ++eye) {
      auto& q = quad[eye];
      q.space                    = s.localSpace;
      q.eyeVisibility            = eye==0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
      q.subImage.swapchain       = s.swapchain[eye];
      q.subImage.imageRect       = {{0,0},{int32_t(s.width),int32_t(s.height)}};
      q.subImage.imageArrayIndex = 0;
      q.pose                     = s.windowPose;
      q.size                     = {s.windowWidth, s.windowHeight()};
      addLayer(q);
      }
    }

  // Frame parts: the grab handle, and the resize bracket of a corner a hand is reaching for
  constexpr XrCompositionLayerFlags chromeFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                               XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
  XrCompositionLayerQuad button = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  if(s.buttonReady && s.roomAvailable() && hasImage) {
    const bool hover = s.grabbed==Grip::Environment || s.hands[0].hover==Grip::Environment || s.hands[1].hover==Grip::Environment;
    button.layerFlags = chromeFlags;
    button.space = s.localSpace;
    button.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    button.subImage.swapchain = s.environmentButton;
    button.subImage.imageRect.extent = {int32_t(Impl::ButtonW), int32_t(Impl::ButtonH)};
    button.subImage.imageArrayIndex = (s.immersive ? 2u : 0u) + (hover ? 1u : 0u);
    button.pose = s.windowPose;
    button.pose.position = toXr(toWorld(s.windowPose, s.buttonCenter()));
    button.size = {0.104f,0.065f};
    addLayer(button);
    }
  XrCompositionLayerQuad slider[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},{XR_TYPE_COMPOSITION_LAYER_QUAD}};
  const int row = s.sliderRow();
  if(hasImage && row>=0) {
    const auto center = s.buttonCenter();
    for(unsigned part=0; part<2; ++part) {
      const bool knob = part==1;
      auto& q = slider[part];
      q.layerFlags = chromeFlags;
      q.space = s.localSpace;
      q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain = s.dimSlider;
      q.subImage.imageRect = {{knob ? Impl::RailW : 0,row*Impl::KnobW},
                              {knob ? Impl::KnobW : Impl::RailW,Impl::KnobW}};
      q.pose = s.windowPose;
      const float x = Impl::DimLeft+Impl::DimTravel*(knob ? s.passthroughDim : 0.5f);
      q.pose.position = toXr(toWorld(s.windowPose,center+V3{x,0.f,0.f}));
      q.size = knob ? XrExtent2Df{0.034f,0.034f} : XrExtent2Df{0.22f,0.033f};
      addLayer(q);
      }
    }
  XrCompositionLayerQuad handle = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::array<XrCompositionLayerQuad,2> corners;
  if(s.handleReady && hasImage) {
    const bool hoverHandle = s.hands[0].hover==Grip::Handle || s.hands[1].hover==Grip::Handle;

    handle.layerFlags                = chromeFlags;
    handle.space                     = s.localSpace;
    handle.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
    handle.subImage.swapchain        = s.handleSwapchain;
    handle.subImage.imageRect        = {{0,0},{int32_t(Impl::HandleW),int32_t(Impl::HandleH)}};
    handle.subImage.imageArrayIndex  = s.grabbed==Grip::Handle ? 2u : (hoverHandle ? 1u : 0u);
    handle.pose.orientation          = s.windowPose.orientation;
    handle.pose.position             = toXr(toWorld(s.windowPose, s.handleCenter()));
    handle.size                      = {0.22f, 0.0275f};
    addLayer(handle);
    }

  if(s.cornersReady && hasImage) {
    const float halfW = s.windowWidth*0.5f, halfH = s.windowHeight()*0.5f, sz = 0.10f;
    for(int c=0; c<2; ++c) {
      const auto corner = c==0 ? Grip::CornerLeft : Grip::CornerRight;
      const bool held = s.grabbed==corner;
      if(!held && s.hands[0].hover!=corner && s.hands[1].hover!=corner)
        continue;
      // The bracket's inner corner sits just outside the window corner
      const float sx = c==0 ? -1.f : 1.f;
      const float k  = sz*(44.f/64.f - 0.5f);   // bracket corner offset from the quad centre
      const float inset = s.cornerInset();
      const V3 at{sx*(halfW - inset + 0.012f - k), -halfH + inset - 0.012f + k, 0.002f};
      auto& q = corners[size_t(c)];
      q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.layerFlags               = chromeFlags;
      q.space                    = s.localSpace;
      q.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain       = s.cornerSwapchain;
      q.subImage.imageRect       = {{0,0},{int32_t(Impl::CornerSize),int32_t(Impl::CornerSize)}};
      q.subImage.imageArrayIndex = uint32_t((c==0 ? 2 : 0) + (held ? 1 : 0));
      q.pose.orientation         = s.windowPose.orientation;
      q.pose.position            = toXr(toWorld(s.windowPose, at));
      q.size                     = {sz, sz};
      addLayer(q);
      }
    }

  XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
  end.displayTime          = s.frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount           = s.frame.shouldRender && isVisible() ? layerCount : 0;
  end.layers               = layers.data();
  {
  const uint64_t t0 = xrUs();
  const bool ended = s.sessionResult(xrEndFrame(s.session, &end), "xrEndFrame");
  tEndFrame += xrUs()-t0;
  if(s.traceSurface && spill!=nullptr) {
    const auto& p=spill->pose.position;
    const auto& q=spill->pose.orientation;
    Log::i("XR surface layer submitted=",int(ended && end.layerCount!=0)," time=",end.displayTime,
           " position=",p.x,",",p.y,",",p.z," orientation=",q.x,",",q.y,",",q.z,",",q.w,
           " size=",spill->size.width,"x",spill->size.height," blend=",s.hasAlphaBlend ? "ONE/ONE" : "premultiplied");
    }
  return ended && warp;
  }
  }

Size XrWindow::resolution() const {
  return Size(int(impl->width), int(impl->height));
  }

Size XrWindow::desiredResolution() const {
  const auto& s = *impl;
  if(!s.eyesValid)
    return resolution();
  const float refHalf = s.verticalFov*float(M_PI)/360.f;
  const float refX = 2.f*std::atan((16.f/9.f)*std::tan(refHalf));
  float ppm = 0.f;
  for(auto& f:s.eyeFov) {
    ppm = std::max(ppm,1600.f*(f.angleRight-f.angleLeft)/(refX*s.windowWidth));
    ppm = std::max(ppm,900.f*(f.angleUp-f.angleDown)/(2.f*refHalf*s.windowHeight()));
    }
  ppm = std::min({ppm,float(s.capacityW)/s.windowWidth,float(s.capacityH)/s.windowHeight()});
  auto multiple = [](float x,int n) { return std::max(n,int(std::round(x/float(n)))*n); };
  const int w = std::min(int(s.capacityW),multiple(s.windowWidth*ppm,32));
  const int h = std::min(int(s.capacityH),multiple(float(w)*s.windowHeight()/s.windowWidth,4));
  const float aspect = s.windowWidth/s.windowHeight();
  if(std::abs(float(s.width)/float(s.height)/aspect-1.f)<0.02f &&
     std::abs(float(w)/float(s.width)-1.f)<0.08f && std::abs(float(h)/float(s.height)-1.f)<0.08f)
    return resolution();
  return Size(w,h);
  }

void XrWindow::setResolution(Size size) {
  auto& s = *impl;
  s.width = uint32_t(size.w);
  s.height = uint32_t(size.h);
  if(!s.fullMotionExtent) {
    s.mvWidth = std::max(s.width/4u,1u);
    s.mvHeight = std::max(s.height/4u,1u);
    }
  }

float XrWindow::densityUiScale() const {
  const auto& s = *impl;
  if(!s.eyesValid)
    return uiScale();
  const float z = std::max(0.05f,(s.eyeInWindow[0].z+s.eyeInWindow[1].z)*0.5f);
  const float scale = uiScale()*(float(s.height)/900.f)*(1.35f/s.windowHeight())*(z/Impl::portalDistance(s.verticalFov));
  return std::clamp(std::round(scale*8.f)/8.f,0.5f,3.f);
  }

std::string XrWindow::timingReport() {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "acquire %.0f swap-wait %.0f release %.0f end %.0f (ms total)",
                double(tAcquire)/1000.0, double(tSwapWait)/1000.0, double(tRelease)/1000.0, double(tEndFrame)/1000.0);
  tAcquire = tSwapWait = tRelease = tEndFrame = 0;
  return buf;
  }

void XrWindow::setVerticalFov(float degrees) {
  impl->verticalFov = degrees;
  }

PadState XrWindow::pad() const {
  return impl->padState;
  }

bool XrWindow::takePadInterruption(PadState& releaseSources) {
  releaseSources = impl->releasePad;
  return std::exchange(impl->padInterrupted,false);
  }

bool XrWindow::isFocused() const {
  return AndroidApi::activityFocused() && impl->state==XR_SESSION_STATE_FOCUSED;
  }

bool XrWindow::isVisible() const {
  return AndroidApi::activityResumed() && impl->running &&
         (impl->state==XR_SESSION_STATE_VISIBLE || impl->state==XR_SESSION_STATE_FOCUSED);
  }

#else

struct XrWindow::Impl {};

XrWindow::XrWindow()  = default;
XrWindow::~XrWindow() = default;

bool      XrWindow::preInit()  { return false; }
XrWindow* XrWindow::inst()     { return nullptr; }
bool      XrWindow::pollInputFocus(bool) { return false; }
void      XrWindow::attach(Tempest::Device&) {}
Tempest::Size XrWindow::resolution() const { return Tempest::Size(0,0); }
Tempest::Size XrWindow::desiredResolution() const { return resolution(); }
void      XrWindow::setResolution(Tempest::Size) {}
float     XrWindow::densityUiScale() const { return uiScale(); }
bool      XrWindow::beginFrame() { return false; }
int64_t   XrWindow::nominalFramePeriod() const { return 0; }
int64_t   XrWindow::predictedDisplayTime() const { return 0; }
bool      XrWindow::submit(bool) { return false; }
bool      XrWindow::isSpaceWarpSupported() const { return false; }
#ifdef OPENGOTHIC_XR
bool      XrWindow::isFoveationSupported() const { return false; }
bool      XrWindow::isWindowFilterSupported() const { return false; }
bool      XrWindow::isSurfaceLightSupported() const { return false; }
void      XrWindow::drawWindowEdges(Tempest::Encoder<Tempest::CommandBuffer>&, bool) {}
void      XrWindow::drawSurfaceLight(Renderer&, Tempest::Encoder<Tempest::CommandBuffer>&, bool) {}
void      XrWindow::prepareLightingRate(Tempest::Encoder<Tempest::CommandBuffer>&, uint8_t, uint8_t, uint32_t, uint32_t) {}
#endif
bool      XrWindow::prepareSpaceWarp(bool, bool) { return false; }
Tempest::Attachment* XrWindow::motionTarget(uint8_t) { return nullptr; }
Tempest::ZBuffer*    XrWindow::depthTarget(uint8_t) { return nullptr; }
Tempest::Attachment* XrWindow::eyeTarget(uint8_t) { return nullptr; }
bool      XrWindow::eyeView(uint8_t, float, Camera::EyeView&) const { return false; }
void      XrWindow::setVerticalFov(float) {}
std::string XrWindow::timingReport() { return ""; }
PadState  XrWindow::pad() const { return {}; }
bool XrWindow::takePadInterruption(PadState&) { return false; }
bool      XrWindow::isFocused() const { return false; }
bool      XrWindow::isVisible() const { return false; }

#endif
