#include "xrwindow.h"

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)

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

#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
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

struct Hand {
  XrPath         path        = XR_NULL_PATH;
  XrSpace        aimSpace    = XR_NULL_HANDLE;
  bool           aimValid    = false;
  XrPosef        aim         = {{0,0,0,1},{0,0,0}};
  float          grab        = 0;
  bool           grabHeld    = false;
  bool           hover       = false;
  V3             hit;
  };

}

struct XrWindow::Impl {
  // Instance
  XrInstance               instance = XR_NULL_HANDLE;
  XrSystemId               system   = XR_NULL_SYSTEM_ID;
  bool                     hasPassthrough     = false;
  bool                     hasHandInteraction = false;

  PFN_xrCreateVulkanInstanceKHR         xrCreateVulkanInstance      = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR     xrGetVulkanGraphicsDevice   = nullptr;
  PFN_xrCreateVulkanDeviceKHR           xrCreateVulkanDevice        = nullptr;
  PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanRequirements   = nullptr;
  PFN_xrCreatePassthroughFB             xrCreatePassthrough         = nullptr;
  PFN_xrDestroyPassthroughFB            xrDestroyPassthrough        = nullptr;
  PFN_xrCreatePassthroughLayerFB        xrCreatePassthroughLayer    = nullptr;
  PFN_xrDestroyPassthroughLayerFB       xrDestroyPassthroughLayer   = nullptr;

  // Session
  VulkanNativeContext      vk;
  XrSession                session    = XR_NULL_HANDLE;
  XrSessionState           state      = XR_SESSION_STATE_UNKNOWN;
  bool                     running    = false;
  XrSpace                  localSpace = XR_NULL_HANDLE;
  XrSpace                  viewSpace  = XR_NULL_HANDLE;
  XrPassthroughFB          passthrough      = XR_NULL_HANDLE;
  XrPassthroughLayerFB     passthroughLayer = XR_NULL_HANDLE;

  // Window image
  uint32_t                 width  = 1600;
  uint32_t                 height = 900;
  XrSwapchain              swapchain = XR_NULL_HANDLE;
  std::vector<VkImage>     images;
  VkCommandPool            pool = VK_NULL_HANDLE;
  static constexpr uint32_t Ring = 3;
  VkCommandBuffer          cmd  [Ring] = {};
  VkFence                  fence[Ring] = {};
  uint32_t                 ringAt = 0;

  // Cursor dot shown where a controller or hand points at the window
  XrSwapchain              cursorSwapchain = XR_NULL_HANDLE;
  bool                     cursorReady     = false;

  // Frame
  XrFrameState             frame = {XR_TYPE_FRAME_STATE};
  bool                     frameBegun = false;

  // Window placement (local space)
  XrPosef                  windowPose = {{0,0,0,1},{0,0,-1.6f}};
  float                    windowWidth = 1.6f;
  bool                     placed      = false;
  int                      grabbedBy   = -1;
  XrPosef                  grabOffset  = {{0,0,0,1},{0,0,0}};
  float                    grabDistance = 0;
  V3                       headPos;

  // Input
  XrActionSet              actionSet  = XR_NULL_HANDLE;
  XrAction                 aAim       = XR_NULL_HANDLE;
  XrAction                 aGrab      = XR_NULL_HANDLE;
  XrAction                 aStick     = XR_NULL_HANDLE;
  XrAction                 aTrigger   = XR_NULL_HANDLE;
  XrAction                 aStickClick= XR_NULL_HANDLE;
  XrAction                 aA = XR_NULL_HANDLE, aB = XR_NULL_HANDLE, aX = XR_NULL_HANDLE, aY = XR_NULL_HANDLE;
  XrAction                 aMenu      = XR_NULL_HANDLE;
  std::array<Hand,2>       hands;
  PadState                 padState;

  ~Impl();

  bool createInstance();
  void createSession();
  void createSwapchains();
  void createPassthrough();
  void createActions();
  void pollEvents();
  void updateInput();
  void placeWindow();
  void fillCursor();
  bool copyImage(const Attachment& img);
  XrPath path(const char* p) const { XrPath r = XR_NULL_PATH; xrStringToPath(instance, p, &r); return r; }
  float  windowHeight() const { return windowWidth*float(height)/float(width); }
  };

static XrWindow* instance = nullptr;

XrWindow::XrWindow() : impl(new Impl()) {
  }

XrWindow::~XrWindow() {
  }

XrWindow* XrWindow::inst() {
  return instance;
  }

void XrWindow::shutdown() {
  delete instance;
  instance = nullptr;
  }

XrWindow::Impl::~Impl() {
  if(vk.device!=VK_NULL_HANDLE) {
    vkDeviceWaitIdle(vk.device);
    for(auto f:fence)
      if(f!=VK_NULL_HANDLE)
        vkDestroyFence(vk.device, f, nullptr);
    if(pool!=VK_NULL_HANDLE)
      vkDestroyCommandPool(vk.device, pool, nullptr);
    }
  if(passthroughLayer!=XR_NULL_HANDLE)
    xrDestroyPassthroughLayer(passthroughLayer);
  if(passthrough!=XR_NULL_HANDLE)
    xrDestroyPassthrough(passthrough);
  if(cursorSwapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(cursorSwapchain);
  if(swapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(swapchain);
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
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data());
  auto has = [&](const char* name) {
    for(auto& p:props)
      if(std::strcmp(p.extensionName,name)==0)
        return true;
    return false;
    };

  std::vector<const char*> ext = {
    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
    };
  for(auto e:ext)
    if(!has(e)) {
      Log::e("OpenXR: missing ", e);
      return false;
      }
  hasPassthrough     = has(XR_FB_PASSTHROUGH_EXTENSION_NAME);
  hasHandInteraction = has(XR_EXT_HAND_INTERACTION_EXTENSION_NAME);
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
  if(!ok(xrCreateInstance(&info, &instance), "xrCreateInstance"))
    return false;

  XrSystemGetInfo sys = {XR_TYPE_SYSTEM_GET_INFO};
  sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  if(!ok(xrGetSystem(instance, &sys, &system), "xrGetSystem"))
    return false;

  auto fn = [this](const char* name, auto& out) {
    return ok(xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&out)), name);
    };
  if(!fn("xrCreateVulkanInstanceKHR",          xrCreateVulkanInstance)    ||
     !fn("xrGetVulkanGraphicsDevice2KHR",      xrGetVulkanGraphicsDevice) ||
     !fn("xrCreateVulkanDeviceKHR",            xrCreateVulkanDevice)      ||
     !fn("xrGetVulkanGraphicsRequirements2KHR",xrGetVulkanRequirements))
    return false;
  if(hasPassthrough) {
    hasPassthrough = fn("xrCreatePassthroughFB",       xrCreatePassthrough)      &&
                     fn("xrDestroyPassthroughFB",      xrDestroyPassthrough)     &&
                     fn("xrCreatePassthroughLayerFB",  xrCreatePassthroughLayer) &&
                     fn("xrDestroyPassthroughLayerFB", xrDestroyPassthroughLayer);
    }

  // Required before the device is created
  XrGraphicsRequirementsVulkan2KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  xrGetVulkanRequirements(instance, system, &req);

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
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    if(!ok(self.xrCreateVulkanDevice(self.instance, &info, out, &vr), "xrCreateVulkanDeviceKHR"))
      return VK_ERROR_INITIALIZATION_FAILED;
    return vr;
    };
  return true;
  }

bool XrWindow::preInit() {
  if(instance!=nullptr)
    return true;
  auto w = new XrWindow();
  if(!w->impl->createInstance()) {
    vulkanCreateHooks() = VulkanCreateHooks();
    delete w;
    Log::i("OpenXR: not available, using the Android window");
    return false;
    }
  instance = w;
  return true;
  }

void XrWindow::attach(Device& device) {
  impl->vk = vulkanNativeContext(device);
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
  if(!ok(xrCreateSession(instance, &info, &session), "xrCreateSession"))
    throw std::runtime_error("OpenXR session");

  XrReferenceSpaceCreateInfo space = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
  space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
  ok(xrCreateReferenceSpace(session, &space, &localSpace), "local space");
  space.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_VIEW;
  ok(xrCreateReferenceSpace(session, &space, &viewSpace), "view space");

  VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = vk.queueFamily;
  vkCreateCommandPool(vk.device, &poolInfo, nullptr, &pool);
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool        = pool;
  alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = Ring;
  vkAllocateCommandBuffers(vk.device, &alloc, cmd);
  for(auto& f:fence) {
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(vk.device, &fi, nullptr, &f);
    }

  createSwapchains();
  createPassthrough();
  createActions();
  }

void XrWindow::Impl::createSwapchains() {
  uint32_t count = 0;
  xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(session, count, &count, formats.data());

  // The game writes gamma-encoded colour into an RGBA8 image; copying those bytes into an
  // sRGB image lets the compositor decode them correctly.
  int64_t format = VK_FORMAT_R8G8B8A8_SRGB;
  bool    found  = false;
  for(auto f:formats)
    if(f==VK_FORMAT_R8G8B8A8_SRGB)
      found = true;
  if(!found) {
    Log::e("OpenXR: RGBA8 sRGB swapchains are not supported, colours will be off");
    format = formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats[0];
    }

  XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format      = format;
  info.sampleCount = 1;
  info.width       = width;
  info.height      = height;
  info.faceCount   = 1;
  info.arraySize   = 1;
  info.mipCount    = 1;
  if(!ok(xrCreateSwapchain(session, &info, &swapchain), "window swapchain"))
    throw std::runtime_error("OpenXR swapchain");

  xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageVulkan2KHR> img(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  xrEnumerateSwapchainImages(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(img.data()));
  for(auto& i:img)
    images.push_back(i.image);

  info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  info.width       = 16;
  info.height      = 16;
  ok(xrCreateSwapchain(session, &info, &cursorSwapchain), "cursor swapchain");
  }

void XrWindow::Impl::createPassthrough() {
  if(!hasPassthrough)
    return;
  XrPassthroughCreateInfoFB info = {XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
  info.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  if(!ok(xrCreatePassthrough(session, &info, &passthrough), "xrCreatePassthroughFB"))
    return;
  XrPassthroughLayerCreateInfoFB layer = {XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
  layer.passthrough = passthrough;
  layer.flags       = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
  layer.purpose     = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
  ok(xrCreatePassthroughLayer(session, &layer, &passthroughLayer), "xrCreatePassthroughLayerFB");
  }

void XrWindow::Impl::createActions() {
  XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(setInfo.actionSetName,          "game");
  std::strcpy(setInfo.localizedActionSetName, "Game");
  ok(xrCreateActionSet(instance, &setInfo, &actionSet), "action set");

  hands[0].path = path("/user/hand/left");
  hands[1].path = path("/user/hand/right");
  const XrPath both[2] = {hands[0].path, hands[1].path};

  auto make = [&](XrAction& out, const char* name, const char* label, XrActionType type, bool perHand) {
    XrActionCreateInfo info = {XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strcpy(info.actionName,          name);
    std::strcpy(info.localizedActionName, label);
    if(perHand) {
      info.countSubactionPaths = 2;
      info.subactionPaths      = both;
      }
    ok(xrCreateAction(actionSet, &info, &out), name);
    };
  make(aAim,        "aim",          "Aim",              XR_ACTION_TYPE_POSE_INPUT,     true);
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
    std::vector<XrActionSuggestedBinding> b;
    for(auto& i:list)
      b.push_back({i.first, path(i.second)});
    XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile     = path(profile);
    s.countSuggestedBindings = uint32_t(b.size());
    s.suggestedBindings      = b.data();
    ok(xrSuggestInteractionProfileBindings(instance, &s), profile);
    };
  suggest("/interaction_profiles/oculus/touch_controller", {
    {aAim,        "/user/hand/left/input/aim/pose"},
    {aAim,        "/user/hand/right/input/aim/pose"},
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
      {aGrab, "/user/hand/left/input/pinch_ext/value"},
      {aGrab, "/user/hand/right/input/pinch_ext/value"},
      });
    }

  for(auto& h:hands) {
    XrActionSpaceCreateInfo info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action            = aAim;
    info.subactionPath     = h.path;
    info.poseInActionSpace = {{0,0,0,1},{0,0,0}};
    ok(xrCreateActionSpace(session, &info, &h.aimSpace), "aim space");
    }

  XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attach.countActionSets = 1;
  attach.actionSets      = &actionSet;
  ok(xrAttachSessionActionSets(session, &attach), "attach action sets");
  }

void XrWindow::Impl::pollEvents() {
  XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
  while(xrPollEvent(instance, &ev)==XR_SUCCESS) {
    switch(ev.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
        state = e.state;
        Log::i("OpenXR: session state ", int(state));
        if(state==XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
          begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          running = ok(xrBeginSession(session, &begin), "xrBeginSession");
          }
        else if(state==XR_SESSION_STATE_STOPPING) {
          xrEndSession(session);
          running = false;
          }
        else if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING) {
          running = false;
          SystemApi::exit();
          }
        break;
        }
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        // The user recentred the view: bring the window back in front of them
        placed = false;
        break;
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        running = false;
        SystemApi::exit();
        break;
      default:
        break;
      }
    ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
  }

void XrWindow::Impl::placeWindow() {
  XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
  if(!XR_SUCCEEDED(xrLocateSpace(viewSpace, localSpace, frame.predictedDisplayTime, &head)))
    return;
  const bool valid = (head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                     (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
  if(!valid)
    return;
  headPos = toV3(head.pose.position);
  if(placed)
    return;

  V3 fwd = rotate(head.pose.orientation, V3{0,0,-1});
  fwd.y  = 0;
  fwd    = normalize(fwd);
  const float distance = 1.6f;
  const V3    pos      = headPos + fwd*distance + V3{0,-0.1f,0};
  windowPose.position    = toXr(pos);
  windowPose.orientation = facing(headPos - pos);
  placed = true;
  }

void XrWindow::Impl::updateInput() {
  padState = PadState();
  if(state!=XR_SESSION_STATE_FOCUSED)
    return;

  XrActiveActionSet active = {actionSet, XR_NULL_PATH};
  XrActionsSyncInfo sync   = {XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets      = &active;
  if(!XR_SUCCEEDED(xrSyncActions(session, &sync)))
    return;

  auto getF = [&](XrAction a, XrPath sub) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a; gi.subactionPath = sub;
    XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(session, &gi, &st);
    return st.isActive ? st.currentState : 0.f;
    };
  auto getB = [&](XrAction a, XrPath sub) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a; gi.subactionPath = sub;
    XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(session, &gi, &st);
    return st.isActive && st.currentState;
    };
  auto getV = [&](XrAction a, XrPath sub) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a; gi.subactionPath = sub;
    XrActionStateVector2f st = {XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(session, &gi, &st);
    return st.isActive ? st.currentState : XrVector2f{0,0};
    };

  // Pointing at and grabbing the window
  const V3    wPos    = toV3(windowPose.position);
  const V3    wNormal = rotate(windowPose.orientation, V3{0,0,1});
  const V3    wRight  = rotate(windowPose.orientation, V3{1,0,0});
  const V3    wUp     = rotate(windowPose.orientation, V3{0,1,0});
  const float halfW   = windowWidth*0.5f;
  const float halfH   = windowHeight()*0.5f;

  for(size_t i=0; i<hands.size(); ++i) {
    auto& h = hands[i];
    XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
    h.aimValid = XR_SUCCEEDED(xrLocateSpace(h.aimSpace, localSpace, frame.predictedDisplayTime, &loc)) &&
                 (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                 (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    h.aim   = loc.pose;
    h.grab  = getF(aGrab, h.path);
    h.hover = false;
    if(!h.aimValid)
      continue;

    const V3    origin = toV3(h.aim.position);
    const V3    dir    = rotate(h.aim.orientation, V3{0,0,-1});
    const float denom  = dot(dir, wNormal);
    if(denom<-1e-4f) {
      const float t = dot(wPos-origin, wNormal)/denom;
      if(t>0) {
        const V3    p = origin + dir*t;
        const float x = dot(p-wPos, wRight);
        const float y = dot(p-wPos, wUp);
        // A small margin around the frame keeps edges easy to catch
        if(std::abs(x)<=halfW+0.05f && std::abs(y)<=halfH+0.05f) {
          h.hover = true;
          h.hit   = p;
          }
        }
      }
    }

  for(size_t i=0; i<hands.size(); ++i) {
    auto& h = hands[i];
    const bool pressed  = h.grab>0.75f;
    const bool released = h.grab<0.35f;
    if(grabbedBy==int(i)) {
      if(released || !h.aimValid) {
        grabbedBy  = -1;
        h.grabHeld = false;
        continue;
        }
      // Keep the window at the same distance along the ray, facing the user, without roll
      const V3 origin = toV3(h.aim.position);
      const V3 dir    = rotate(h.aim.orientation, V3{0,0,-1});
      // Thumbstick pushes the window away or pulls it closer
      const XrVector2f stick = getV(aStick, h.path);
      grabDistance = std::clamp(grabDistance + stick.y*0.02f, 0.4f, 6.f);
      windowWidth  = std::clamp(windowWidth*(1.f + stick.x*0.01f), 0.4f, 6.f);
      const V3 center = origin + dir*grabDistance - rotate(windowPose.orientation, toV3(grabOffset.position));
      windowPose.position    = toXr(center);
      windowPose.orientation = facing(headPos - center);
      h.hit   = origin + dir*grabDistance;
      h.hover = true;
      }
    else if(grabbedBy<0 && pressed && !h.grabHeld && h.hover) {
      grabbedBy    = int(i);
      grabDistance = length(h.hit - toV3(h.aim.position));
      // Offset of the grab point from the window centre, in window space
      const V3 d = h.hit - wPos;
      grabOffset.position = toXr(V3{dot(d,wRight), dot(d,wUp), 0});
      }
    h.grabHeld = pressed ? true : (released ? false : h.grabHeld);
    }

  if(grabbedBy>=0)
    return; // the sticks adjust the window while it is held

  const XrVector2f ls = getV(aStick, hands[0].path);
  const XrVector2f rs = getV(aStick, hands[1].path);
  padState.connected = true;
  padState.lx =  ls.x;
  padState.ly = -ls.y;
  padState.rx =  rs.x;
  padState.ry = -rs.y;
  padState.lt = getF(aTrigger, hands[0].path);
  padState.rt = getF(aTrigger, hands[1].path);
  padState.buttons[PadState::A]          = getB(aA, XR_NULL_PATH);
  padState.buttons[PadState::B]          = getB(aB, XR_NULL_PATH);
  padState.buttons[PadState::X]          = getB(aX, XR_NULL_PATH);
  padState.buttons[PadState::Y]          = getB(aY, XR_NULL_PATH);
  padState.buttons[PadState::Start]      = getB(aMenu, XR_NULL_PATH);
  padState.buttons[PadState::LeftStick]  = getB(aStickClick, hands[0].path);
  padState.buttons[PadState::RightStick] = getB(aStickClick, hands[1].path);
  }

bool XrWindow::beginFrame() {
  auto& s = *impl;
  s.pollEvents();
  if(!s.running) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return false;
    }

  XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
  s.frame = {XR_TYPE_FRAME_STATE};
  if(!ok(xrWaitFrame(s.session, &wait, &s.frame), "xrWaitFrame"))
    return false;
  {
  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
  if(!ok(xrBeginFrame(s.session, &begin), "xrBeginFrame"))
    return false;
  }
  s.frameBegun = true;

  s.placeWindow();
  s.updateInput();

  if(!s.frame.shouldRender) {
    submit(nullptr);
    return false;
    }
  return true;
  }

void XrWindow::Impl::fillCursor() {
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if(!XR_SUCCEEDED(xrAcquireSwapchainImage(cursorSwapchain, &acq, &index)))
    return;
  XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  xrWaitSwapchainImage(cursorSwapchain, &wait);

  uint32_t count = 0;
  xrEnumerateSwapchainImages(cursorSwapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageVulkan2KHR> img(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  xrEnumerateSwapchainImages(cursorSwapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(img.data()));

  VkCommandBuffer cb = cmd[0];
  vkWaitForFences(vk.device, 1, &fence[0], VK_TRUE, UINT64_MAX);
  vkResetFences(vk.device, 1, &fence[0]);
  vkResetCommandBuffer(cb, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);

  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = img[index].image;
  b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  VkClearColorValue color = {{1.f, 0.85f, 0.45f, 1.f}};
  vkCmdClearColorImage(cb, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &b.subresourceRange);
  b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = 0;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  vkEndCommandBuffer(cb);

  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb;
  vkQueueSubmit(vk.queue, 1, &si, fence[0]);

  XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(cursorSwapchain, &rel);
  cursorReady = true;
  }

bool XrWindow::Impl::copyImage(const Attachment& src) {
  const VulkanNativeImage from = vulkanNativeImage(src);
  if(from.image==VK_NULL_HANDLE || src.w()!=int(width) || src.h()!=int(height))
    return false;

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if(!ok(xrAcquireSwapchainImage(swapchain, &acq, &index), "acquire window image"))
    return false;
  XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  xrWaitSwapchainImage(swapchain, &wait);

  const uint32_t  slot = ringAt;
  ringAt = (ringAt+1)%Ring;
  VkCommandBuffer cb = cmd[slot];
  vkWaitForFences(vk.device, 1, &fence[slot], VK_TRUE, UINT64_MAX);
  vkResetFences(vk.device, 1, &fence[slot]);
  vkResetCommandBuffer(cb, 0);

  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);

  VkImageMemoryBarrier b[2] = {};
  for(auto& i:b) {
    i.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    i.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    i.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    i.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }
  // The game's frame was submitted earlier on this queue; wait for its writes
  b[0].image         = from.image;
  b[0].oldLayout     = from.restingLayout;
  b[0].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[1].image         = images[index];
  b[1].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
  b[1].newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].srcAccessMask = 0;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);

  VkImageCopy region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent         = {width, height, 1};
  vkCmdCopyImage(cb, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  // Hand the image back to the game in its resting layout and the swapchain image to the compositor
  b[0].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].newLayout     = from.restingLayout;
  b[0].srcAccessMask = 0;
  b[0].dstAccessMask = 0;
  b[1].oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].dstAccessMask = 0;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
  vkEndCommandBuffer(cb);

  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb;
  vkQueueSubmit(vk.queue, 1, &si, fence[slot]);

  XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(swapchain, &rel);
  return true;
  }

void XrWindow::submit(const Attachment* image) {
  auto& s = *impl;
  if(!s.frameBegun)
    return;
  s.frameBegun = false;

  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  if(!s.cursorReady && s.cursorSwapchain!=XR_NULL_HANDLE)
    s.fillCursor();
  const bool hasImage = image!=nullptr && s.copyImage(*image);

  std::vector<const XrCompositionLayerBaseHeader*> layers;

  XrCompositionLayerPassthroughFB pt = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  if(s.passthroughLayer!=XR_NULL_HANDLE) {
    pt.layerHandle = s.passthroughLayer;
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pt));
    }

  XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  if(hasImage) {
    quad.space                     = s.localSpace;
    quad.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain        = s.swapchain;
    quad.subImage.imageRect        = {{0,0},{int32_t(s.width),int32_t(s.height)}};
    quad.subImage.imageArrayIndex  = 0;
    quad.pose                      = s.windowPose;
    quad.size                      = {s.windowWidth, s.windowHeight()};
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
    }

  std::array<XrCompositionLayerQuad,2> cursors;
  if(s.cursorReady) {
    for(size_t i=0; i<s.hands.size(); ++i) {
      auto& h = s.hands[i];
      if(!h.hover || !hasImage)
        continue;
      auto& c = cursors[i];
      c = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      c.space                    = s.localSpace;
      c.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
      c.subImage.swapchain       = s.cursorSwapchain;
      c.subImage.imageRect       = {{0,0},{16,16}};
      c.pose.orientation         = s.windowPose.orientation;
      // Slightly in front of the window so it never sinks into the image
      c.pose.position            = toXr(h.hit + rotate(s.windowPose.orientation, V3{0,0,0.005f}));
      const float sz             = s.grabbedBy==int(i) ? 0.018f : 0.012f;
      c.size                     = {sz, sz};
      layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&c));
      }
    }

  XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
  end.displayTime          = s.frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount           = s.frame.shouldRender ? uint32_t(layers.size()) : 0;
  end.layers               = layers.data();
  ok(xrEndFrame(s.session, &end), "xrEndFrame");
  }

Size XrWindow::resolution() const {
  return Size(int(impl->width), int(impl->height));
  }

PadState XrWindow::pad() const {
  return impl->padState;
  }

bool XrWindow::isFocused() const {
  return impl->state==XR_SESSION_STATE_FOCUSED;
  }

#else

struct XrWindow::Impl {};

XrWindow::XrWindow()  = default;
XrWindow::~XrWindow() = default;

bool      XrWindow::preInit()  { return false; }
XrWindow* XrWindow::inst()     { return nullptr; }
void      XrWindow::shutdown() {}
void      XrWindow::attach(Tempest::Device&) {}
Tempest::Size XrWindow::resolution() const { return {}; }
bool      XrWindow::beginFrame() { return false; }
void      XrWindow::submit(const Tempest::Attachment*) {}
PadState  XrWindow::pad() const { return {}; }
bool      XrWindow::isFocused() const { return false; }

#endif
