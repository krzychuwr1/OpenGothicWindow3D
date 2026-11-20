#include "xrwindow.h"
#include "commandline.h"

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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
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
enum class Grip : uint8_t { None, Handle, CornerLeft, CornerRight };

struct Hand {
  XrPath         path        = XR_NULL_PATH;
  XrSpace        aimSpace    = XR_NULL_HANDLE;   // pointing ray
  XrSpace        nearSpace   = XR_NULL_HANDLE;   // pinch point (hands) or grip (controllers), for direct touch
  bool           aimValid    = false;
  bool           nearValid   = false;
  XrPosef        aim         = {{0,0,0,1},{0,0,0}};
  XrPosef        nearPose    = {{0,0,0,1},{0,0,0}};
  float          grab        = 0;
  bool           grabHeld    = false;
  Grip           hover       = Grip::None;
  bool           hoverNear   = false;
  V3             hit;                            // where the hand targets the frame part, local space
  };

}

struct XrWindow::Impl {
  // Instance
  XrInstance               instance = XR_NULL_HANDLE;
  XrSystemId               system   = XR_NULL_SYSTEM_ID;
  bool                     hasPassthrough     = false;
  bool                     hasHandInteraction = false;
  bool                     hasPerfSettings    = false;
  bool                     hasSpaceWarp       = false;
  PFN_xrPerfSettingsSetPerformanceLevelEXT xrSetPerfLevel = nullptr;

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

  // Window image, one swapchain per eye
  uint32_t                 width  = 1600;
  uint32_t                 height = 900;
  XrSwapchain              swapchain[2] = {};
  std::vector<VkImage>     images[2];
  Tempest::Device*         device = nullptr;
  // The game renders straight into the swapchain images through these
  std::vector<Attachment>  targets[2];
  int32_t                  acquired[2] = {-1, -1};

  // Application SpaceWarp
  uint32_t                 mvWidth = 0, mvHeight = 0;
  XrSwapchain              mvSwapchain[2]    = {};
  XrSwapchain              depthSwapchain[2] = {};
  std::vector<Attachment>  mvTargets[2];
  std::vector<ZBuffer>     depthTargets[2];
  int32_t                  mvAcquired[2]    = {-1, -1};
  int32_t                  depthAcquired[2] = {-1, -1};
  bool                     spaceWarpWanted  = false;
  XrPosef                  eyePose[2] = {};
  XrFovf                   eyeFov [2] = {};
  float                    eyeNear[2] = {};
  void createSpaceWarpSwapchains();
  void releaseSpaceWarp();
  VkCommandPool            pool = VK_NULL_HANDLE;
  static constexpr uint32_t Ring = 3;
  VkCommandBuffer          cmd  [Ring] = {};
  VkFence                  fence[Ring] = {};
  uint32_t                 ringAt = 0;

  // Window frame parts: grab handle (idle/hover/held) and resize corners (left/right, hover/held)
  XrSwapchain              handleSwapchain = XR_NULL_HANDLE;
  XrSwapchain              cornerSwapchain = XR_NULL_HANDLE;
  bool                     chromeAttempted = false;
  bool                     chromeReady     = false;
  static constexpr uint32_t HandleW = 256, HandleH = 32, CornerSize = 64;

  // Frame
  XrFrameState             frame = {XR_TYPE_FRAME_STATE};
  bool                     frameBegun = false;
  bool                     eyesValid  = false;
  V3                       eyeInWindow[2];   // metres, window space: +x right, +y up, +z towards the viewer

  // Window placement (local space)
  XrPosef                  windowPose = {{0,0,0,1},{0,0,-1.5f}};
  float                    windowWidth = 2.4f;
  float                    verticalFov = 67.5f;
  bool                     placed      = false;
  XrTime                   recenterTime = 0;
  int                      grabbedBy   = -1;
  Grip                     grabbed     = Grip::None;
  bool                     grabNear    = false;
  V3                       grabLocal;          // grabbed point in the space of the holding pose
  float                    grabScale   = 1;    // window width per corner reach at the start of a resize
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
  PadState                 padState;

  ~Impl();

  bool createInstance();
  void createSession();
  void createSwapchains();
  void createPassthrough();
  void createActions();
  void pollEvents();
  void updateInput();
  void cancelInput();
  void placeWindow();
  void fillChrome();
  Grip targetAt(float x, float y) const;
  V3   handleCenter() const;
  void releaseEyes();
  void locateEyes();
  XrPath path(const char* p) const { XrPath r = XR_NULL_PATH; xrStringToPath(instance, p, &r); return r; }
  float  windowHeight() const { return windowWidth*float(height)/float(width); }
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
    vkDeviceWaitIdle(vk.device);
    for(int eye=0; eye<2; ++eye) {
      targets[eye].clear();
      mvTargets[eye].clear();
      depthTargets[eye].clear();
      }
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
  if(handleSwapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(handleSwapchain);
  if(cornerSwapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(cornerSwapchain);
  for(auto sw:swapchain)
    if(sw!=XR_NULL_HANDLE)
      xrDestroySwapchain(sw);
  for(int eye=0; eye<2; ++eye) {
    if(mvSwapchain[eye]!=XR_NULL_HANDLE)
      xrDestroySwapchain(mvSwapchain[eye]);
    if(depthSwapchain[eye]!=XR_NULL_HANDLE)
      xrDestroySwapchain(depthSwapchain[eye]);
    }
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
  hasPerfSettings    = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
  hasSpaceWarp       = has(XR_FB_SPACE_WARP_EXTENSION_NAME);
  if(hasSpaceWarp)
    ext.push_back(XR_FB_SPACE_WARP_EXTENSION_NAME);
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
  if(hasPerfSettings)
    hasPerfSettings = fn("xrPerfSettingsSetPerformanceLevelEXT", xrSetPerfLevel);
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
  impl->vk     = vulkanNativeContext(device);
  impl->device = &device;
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
  if(vkCreateCommandPool(vk.device, &poolInfo, nullptr, &pool)!=VK_SUCCESS)
    throw std::runtime_error("OpenXR upload command pool");
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool        = pool;
  alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = Ring;
  if(vkAllocateCommandBuffers(vk.device, &alloc, cmd)!=VK_SUCCESS)
    throw std::runtime_error("OpenXR upload command buffers");
  for(auto& f:fence) {
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if(vkCreateFence(vk.device, &fi, nullptr, &f)!=VK_SUCCESS)
      throw std::runtime_error("OpenXR upload fence");
    }

  createSwapchains();
  createSpaceWarpSwapchains();
  createPassthrough();
  createActions();
  }

void XrWindow::Impl::createSwapchains() {
  uint32_t count = 0;
  xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(session, count, &count, formats.data());

  // The renderer and static uploads both require four RGBA8 channels.
  auto supported = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f)!=formats.end(); };
  const int64_t format = supported(VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  if(!supported(format))
    throw std::runtime_error("OpenXR requires an RGBA8 window swapchain");

  XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  // Mutable format: the game writes its gamma-encoded RGBA8 through a UNORM view of the sRGB image
  info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
  info.format      = format;
  info.sampleCount = 1;
  info.width       = width;
  info.height      = height;
  info.faceCount   = 1;
  info.arraySize   = 1;
  info.mipCount    = 1;
  for(int eye=0; eye<2; ++eye) {
    if(!ok(xrCreateSwapchain(session, &info, &swapchain[eye]), "window swapchain"))
      throw std::runtime_error("OpenXR swapchain");
    xrEnumerateSwapchainImages(swapchain[eye], 0, &count, nullptr);
    std::vector<XrSwapchainImageVulkan2KHR> img(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    xrEnumerateSwapchainImages(swapchain[eye], count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(img.data()));
    for(auto& i:img) {
      images[eye].push_back(i.image);
      targets[eye].push_back(vulkanBorrowColorAttachment(*device, i.image, width, height));
      }
    }

  info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.width       = HandleW;
  info.height      = HandleH;
  info.arraySize   = 3;
  ok(xrCreateSwapchain(session, &info, &handleSwapchain), "handle swapchain");
  info.width       = CornerSize;
  info.height      = CornerSize;
  info.arraySize   = 4;
  ok(xrCreateSwapchain(session, &info, &cornerSwapchain), "corner swapchain");
  }

void XrWindow::Impl::createSpaceWarpSwapchains() {
  if(!hasSpaceWarp)
    return;
  // Motion vectors at a quarter of the window resolution (the runtime upsamples them)
  mvWidth  = std::max(width/4u,  16u);
  mvHeight = std::max(height/4u, 16u);

  uint32_t count = 0;
  xrEnumerateSwapchainFormats(session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  xrEnumerateSwapchainFormats(session, count, &count, formats.data());
  auto supported = [&](int64_t f) { return std::find(formats.begin(), formats.end(), f)!=formats.end(); };
  if(!supported(VK_FORMAT_R16G16B16A16_SFLOAT) || !supported(VK_FORMAT_D32_SFLOAT)) {
    Log::e("OpenXR: SpaceWarp needs RGBA16F and D32F swapchains");
    hasSpaceWarp = false;
    return;
    }

  for(int eye=0; eye<2; ++eye) {
    XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.sampleCount = 1;
    info.width       = mvWidth;
    info.height      = mvHeight;
    info.faceCount   = 1;
    info.arraySize   = 1;
    info.mipCount    = 1;
    if(!ok(xrCreateSwapchain(session, &info, &mvSwapchain[eye]), "motion vector swapchain")) {
      hasSpaceWarp = false;
      return;
      }
    info.usageFlags  = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format      = VK_FORMAT_D32_SFLOAT;
    if(!ok(xrCreateSwapchain(session, &info, &depthSwapchain[eye]), "depth swapchain")) {
      hasSpaceWarp = false;
      return;
      }
    auto images = [&](XrSwapchain sc) {
      xrEnumerateSwapchainImages(sc, 0, &count, nullptr);
      std::vector<XrSwapchainImageVulkan2KHR> img(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
      xrEnumerateSwapchainImages(sc, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(img.data()));
      return img;
      };
    for(auto& i:images(mvSwapchain[eye]))
      mvTargets[eye].push_back(vulkanBorrowColorAttachment(*device, i.image, mvWidth, mvHeight, TextureFormat::RGBA16F));
    for(auto& i:images(depthSwapchain[eye]))
      depthTargets[eye].push_back(vulkanBorrowDepthAttachment(*device, i.image, mvWidth, mvHeight, TextureFormat::Depth32F));
    }
  Log::i("OpenXR: SpaceWarp available, motion vectors ", mvWidth, "x", mvHeight);
  }

void XrWindow::Impl::releaseSpaceWarp() {
  for(int eye=0; eye<2; ++eye) {
    XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if(mvAcquired[eye]>=0)
      xrReleaseSwapchainImage(mvSwapchain[eye], &rel);
    if(depthAcquired[eye]>=0)
      xrReleaseSwapchainImage(depthSwapchain[eye], &rel);
    mvAcquired[eye]    = -1;
    depthAcquired[eye] = -1;
    }
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

  for(auto& h:hands) {
    XrActionSpaceCreateInfo info = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    info.action            = aAim;
    info.subactionPath     = h.path;
    info.poseInActionSpace = {{0,0,0,1},{0,0,0}};
    ok(xrCreateActionSpace(session, &info, &h.aimSpace), "aim space");
    info.action            = aNear;
    ok(xrCreateActionSpace(session, &info, &h.nearSpace), "touch space");
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
        if(state!=XR_SESSION_STATE_FOCUSED)
          cancelInput();
        Log::i("OpenXR: session state ", int(state));
        if(state==XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin = {XR_TYPE_SESSION_BEGIN_INFO};
          begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          running = ok(xrBeginSession(session, &begin), "xrBeginSession");
          if(running && hasPerfSettings) {
            // The game is heavy on both: ask for the highest clocks the headset can sustain,
            // or the lowest ones for unattended test runs (-perf low)
            const bool low = CommandLine::inst().xrPerfLevel()=="low";
            const auto lvl = low ? XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
            ok(xrSetPerfLevel(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, lvl), "CPU level");
            ok(xrSetPerfLevel(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, lvl), "GPU level");
            }
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
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
        auto& change = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
        if(change.session==session && change.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL)
          recenterTime = change.changeTime;
        break;
        }
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
  if(recenterTime!=0 && frame.predictedDisplayTime>=recenterTime) {
    recenterTime = 0;
    placed = false;
    cancelInput();
    }
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
  // The sweet spot: the window covers exactly the game's field of view, so nothing is cropped
  const float distance = std::clamp(windowHeight()*0.5f/std::tan(verticalFov*0.5f*float(M_PI)/180.f), 0.6f, 3.f);
  const V3    pos      = headPos + fwd*distance + V3{0,-0.1f,0};
  windowPose.position    = toXr(pos);
  windowPose.orientation = facing(headPos - pos);
  placed = true;
  }

void XrWindow::Impl::cancelInput() {
  padState = {};
  grabbedBy = -1;
  grabbed = Grip::None;
  for(auto& h:hands) {
    h.hover = Grip::None;
    h.grabHeld = true; // require release before a new grab
    }
  }

void XrWindow::Impl::updateInput() {
  padState = PadState();
  if(state!=XR_SESSION_STATE_FOCUSED)
    return;

  XrActiveActionSet active = {actionSet, XR_NULL_PATH};
  XrActionsSyncInfo sync   = {XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets      = &active;
  if(xrSyncActions(session, &sync)!=XR_SUCCESS) {
    cancelInput();
    return;
    }

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

  // Pointing at or touching the frame parts: the handle moves the window, the corners resize it.
  // The game image itself is never a grab target, so playing never moves the window by accident.
  const V3 wNormal = rotate(windowPose.orientation, V3{0,0,1});

  for(size_t i=0; i<hands.size(); ++i) {
    auto& h = hands[i];
    auto locate = [&](XrSpace sp, XrPosef& out) {
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
    const V3    origin = toV3(h.aim.position);
    const V3    dir    = rotate(h.aim.orientation, V3{0,0,-1});
    const float denom  = dot(dir, wNormal);
    if(denom>-1e-4f)
      continue;
    const float t = dot(toV3(windowPose.position)-origin, wNormal)/denom;
    if(t<=0)
      continue;
    const V3 p = origin + dir*t;
    const V3 l = toLocal(windowPose, p);
    const Grip g = targetAt(l.x, l.y);
    if(g!=Grip::None) {
      h.hover = g;
      h.hit   = p;
      }
    }

  for(size_t i=0; i<hands.size(); ++i) {
    auto& h = hands[i];
    const bool pressed  = h.grab>0.75f;
    const bool released = h.grab<0.35f;
    if(grabbedBy==int(i)) {
      const bool tracked = grabNear ? h.nearValid : h.aimValid;
      if(released || !tracked) {
        grabbedBy  = -1;
        grabbed    = Grip::None;
        h.grabHeld = false;
        continue;
        }
      const XrPosef& holder = grabNear ? h.nearPose : h.aim;
      if(!grabNear) {
        // Thumbstick pushes a pointed-at window away or pulls it closer
        const XrVector2f stick = getV(aStick, h.path);
        grabLocal.z = std::clamp(grabLocal.z - stick.y*0.03f, -8.f, -0.2f);
        }
      const V3 point = toWorld(holder, grabLocal);
      h.hit   = point;
      h.hover = grabbed;

      if(grabbed==Grip::Handle) {
        // The handle stays in the hand; the window turns to face the viewer, without roll
        const V3 offset = handleCenter();
        V3 center = point - rotate(windowPose.orientation, offset);
        windowPose.orientation = facing(headPos - center);
        center = point - rotate(windowPose.orientation, offset);
        windowPose.position = toXr(center);
        } else {
        // A corner follows the hand across the window plane; the centre stays put
        const V3    l     = toLocal(windowPose, point);
        const float reach = std::max(std::abs(l.x), std::abs(l.y)*float(width)/float(height));
        windowWidth = std::clamp(reach*grabScale, 0.6f, 6.f);
        }
      }
    else if(grabbedBy<0 && pressed && !h.grabHeld && h.hover!=Grip::None) {
      grabbedBy = int(i);
      grabbed   = h.hover;
      grabNear  = h.hoverNear;
      grabLocal = toLocal(grabNear ? h.nearPose : h.aim, h.hit);
      if(grabbed!=Grip::Handle) {
        const V3    l     = toLocal(windowPose, h.hit);
        const float reach = std::max(std::max(std::abs(l.x), std::abs(l.y)*float(width)/float(height)), 0.05f);
        grabScale = windowWidth/reach;
        }
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

V3 XrWindow::Impl::handleCenter() const {
  return V3{0, -windowHeight()*0.5f - 0.05f, 0};
  }

Grip XrWindow::Impl::targetAt(float x, float y) const {
  const float halfW = windowWidth*0.5f;
  const float halfH = windowHeight()*0.5f;
  // Corners: a hand-sized zone around each bottom corner, mostly outside the image
  const float zone = 0.10f;
  if(std::abs(y+halfH)<zone) {
    if(std::abs(x-halfW)<zone)
      return Grip::CornerRight;
    if(std::abs(x+halfW)<zone)
      return Grip::CornerLeft;
    }
  const V3 hc = handleCenter();
  if(std::abs(x-hc.x)<0.16f && std::abs(y-hc.y)<0.045f)
    return Grip::Handle;
  return Grip::None;
  }

void XrWindow::Impl::locateEyes() {
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
  // Life size: one metre in the room is one metre (100 units) in the game. The window maps onto a
  // plane in front of the game camera, placed so that an eye at the matching distance sees exactly
  // the game's own field of view; any other eye position sees the scene through the window with
  // parallax. Geometry between the eye and that plane is clipped, so nothing pokes through the frame.
  const float units = 100.f;
  const float halfW = s.windowWidth*0.5f*units;
  const float halfH = s.windowHeight()*0.5f*units;
  const float plane = halfH/std::tan(verticalFovDeg*0.5f*float(M_PI)/180.f);

  V3 e = s.eyeInWindow[eye];
  e.z  = std::max(e.z, 0.05f);   // behind or inside the window: hold the eye just in front of it
  // View space of the game camera: +x right, +y down (Vulkan), +z forward
  const float ex = e.x*units;
  const float ey = -e.y*units;
  const float dz = e.z*units;    // eye distance to the window plane
  out.offset = Tempest::Vec3(ex, ey, plane-dz);
  out.zNear  = dz;
  out.left   = -halfW - ex;
  out.right  =  halfW - ex;
  out.bottom = -halfH - ey;
  out.top    =  halfH - ey;
  return true;
  }

void XrWindow::pollEvents() {
  impl->pollEvents();
  }

bool XrWindow::beginFrame() {
  auto& s = *impl;
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

  if(!s.frame.shouldRender || !isVisible()) {
    submit(false);
    return false;
    }
  s.placeWindow();
  s.updateInput();
  s.locateEyes();
  return true;
  }

// Premultiplied RGBA8 pixel with coverage `a` of colour (r,g,b) at opacity `o`
static uint32_t pixel(float r, float g, float b, float o, float a) {
  a = std::clamp(a, 0.f, 1.f)*o;
  auto c = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f)*255.f + 0.5f); };
  return c(r*a) | (c(g*a)<<8) | (c(b*a)<<16) | (c(a)<<24);
  }

static bool uploadStatic(const VulkanNativeContext& vk, VkCommandBuffer cb, VkFence fence, XrSwapchain sc,
                         uint32_t w, uint32_t h, uint32_t layers, const std::vector<uint32_t>& rgba) {
  uint32_t count = 0;
  if(!ok(xrEnumerateSwapchainImages(sc, 0, &count, nullptr), "enumerate static images") || count==0)
    return false;
  std::vector<XrSwapchainImageVulkan2KHR> img(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  if(!ok(xrEnumerateSwapchainImages(sc, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(img.data())), "static images"))
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
  if(vkResetFences(vk.device, 1, &fence)!=VK_SUCCESS || vkQueueSubmit(vk.queue, 1, &si, fence)!=VK_SUCCESS ||
     vkWaitForFences(vk.device, 1, &fence, VK_TRUE, UINT64_MAX)!=VK_SUCCESS)
    return false;

  XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  return ok(xrReleaseSwapchainImage(sc, &rel), "release static image");
  }

void XrWindow::Impl::fillChrome() {
  chromeAttempted = true;
  if(handleSwapchain==XR_NULL_HANDLE || cornerSwapchain==XR_NULL_HANDLE)
    return;

  // Handle: a capsule; idle, hover and held states
  {
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
    if(!uploadStatic(vk, cmd[0], fence[0], handleSwapchain, w, h, 3, px))
      return;
  }

  // Corners: an L bracket around the bottom-right corner (layers 0,1) and its mirror (2,3)
  {
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
    chromeReady = uploadStatic(vk, cmd[0], fence[0], cornerSwapchain, n, n, 4, px);
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
  return &s.targets[eye][size_t(s.acquired[eye])];
  }

bool XrWindow::isSpaceWarpSupported() const {
  return impl->hasSpaceWarp;
  }

void XrWindow::setSpaceWarp(bool use) {
  impl->spaceWarpWanted = use && impl->hasSpaceWarp;
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
  return &s.mvTargets[eye][size_t(s.mvAcquired[eye])];
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
  return &s.depthTargets[eye][size_t(s.depthAcquired[eye])];
  }

void XrWindow::Impl::releaseEyes() {
  const uint64_t t0 = xrUs();
  for(int eye=0; eye<2; ++eye) {
    if(acquired[eye]<0)
      continue;
    // The game's commands for this image are already submitted to the queue
    XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain[eye], &rel);
    acquired[eye] = -1;
    }
  tRelease += xrUs()-t0;
  }

bool XrWindow::submit(bool rendered) {
  auto& s = *impl;
  if(!s.frameBegun)
    return false;
  s.frameBegun = false;

  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  if(rendered && !s.chromeAttempted)
    s.fillChrome();
  const bool hasImage = rendered && s.acquired[0]>=0 && s.acquired[1]>=0;
  const bool warp     = hasImage && s.spaceWarpWanted &&
                        s.mvAcquired[0]>=0 && s.mvAcquired[1]>=0 && s.depthAcquired[0]>=0 && s.depthAcquired[1]>=0;
  s.releaseEyes();
  s.releaseSpaceWarp();

  std::vector<const XrCompositionLayerBaseHeader*> layers;

  XrCompositionLayerPassthroughFB pt = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  if(s.passthroughLayer!=XR_NULL_HANDLE) {
    pt.layerHandle = s.passthroughLayer;
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&pt));
    }

  // The same rectangle in the room for both eyes, each showing its own view through it
  XrCompositionLayerQuad quad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},{XR_TYPE_COMPOSITION_LAYER_QUAD}};
  // SpaceWarp: the window as a projection layer with motion vectors and depth
  XrCompositionLayerProjection      proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView  pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerSpaceWarpInfoFB sw[2] = {{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB},{XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB}};
  if(warp) {
    for(int eye=0; eye<2; ++eye) {
      auto& w = sw[eye];
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
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj));
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
      layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
      }
    }

  // Frame parts: the grab handle, and the resize bracket of a corner a hand is reaching for
  XrCompositionLayerQuad handle = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::array<XrCompositionLayerQuad,2> corners;
  if(s.chromeReady && hasImage) {
    bool hoverHandle = false, cornerOn[2] = {}, cornerHeld[2] = {};
    for(auto& h:s.hands) {
      hoverHandle |= (h.hover==Grip::Handle);
      if(h.hover==Grip::CornerLeft)  cornerOn[0] = true;
      if(h.hover==Grip::CornerRight) cornerOn[1] = true;
      }
    if(s.grabbed==Grip::CornerLeft)  cornerHeld[0] = cornerOn[0] = true;
    if(s.grabbed==Grip::CornerRight) cornerHeld[1] = cornerOn[1] = true;

    handle.layerFlags                = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    handle.space                     = s.localSpace;
    handle.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
    handle.subImage.swapchain        = s.handleSwapchain;
    handle.subImage.imageRect        = {{0,0},{int32_t(Impl::HandleW),int32_t(Impl::HandleH)}};
    handle.subImage.imageArrayIndex  = s.grabbed==Grip::Handle ? 2u : (hoverHandle ? 1u : 0u);
    handle.pose.orientation          = s.windowPose.orientation;
    handle.pose.position             = toXr(toWorld(s.windowPose, s.handleCenter()));
    handle.size                      = {0.22f, 0.0275f};
    layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&handle));

    const float halfW = s.windowWidth*0.5f, halfH = s.windowHeight()*0.5f, sz = 0.10f;
    for(int c=0; c<2; ++c) {
      if(!cornerOn[c])
        continue;
      // The bracket's inner corner sits just outside the window corner
      const float sx = c==0 ? -1.f : 1.f;
      const float k  = sz*(44.f/64.f - 0.5f);   // bracket corner offset from the quad centre
      const V3 at{sx*(halfW + 0.012f - k), -halfH - 0.012f + k, 0.002f};
      auto& q = corners[size_t(c)];
      q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      q.space                    = s.localSpace;
      q.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain       = s.cornerSwapchain;
      q.subImage.imageRect       = {{0,0},{int32_t(Impl::CornerSize),int32_t(Impl::CornerSize)}};
      q.subImage.imageArrayIndex = uint32_t((c==0 ? 2 : 0) + (cornerHeld[c] ? 1 : 0));
      q.pose.orientation         = s.windowPose.orientation;
      q.pose.position            = toXr(toWorld(s.windowPose, at));
      q.size                     = {sz, sz};
      layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q));
      }
    }

  XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
  end.displayTime          = s.frame.predictedDisplayTime;
  end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end.layerCount           = s.frame.shouldRender && isVisible() ? uint32_t(layers.size()) : 0;
  end.layers               = layers.data();
  {
  const uint64_t t0 = xrUs();
  const bool ended = ok(xrEndFrame(s.session, &end), "xrEndFrame");
  tEndFrame += xrUs()-t0;
  return ended && warp;
  }
  }

Size XrWindow::resolution() const {
  return Size(int(impl->width), int(impl->height));
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

bool XrWindow::isFocused() const {
  return impl->state==XR_SESSION_STATE_FOCUSED;
  }

bool XrWindow::isVisible() const {
  return impl->running && (impl->state==XR_SESSION_STATE_VISIBLE || impl->state==XR_SESSION_STATE_FOCUSED);
  }

#else

struct XrWindow::Impl {};

XrWindow::XrWindow()  = default;
XrWindow::~XrWindow() = default;

bool      XrWindow::preInit()  { return false; }
XrWindow* XrWindow::inst()     { return nullptr; }
void      XrWindow::pollEvents() {}
void      XrWindow::attach(Tempest::Device&) {}
Tempest::Size XrWindow::resolution() const { return {}; }
bool      XrWindow::beginFrame() { return false; }
bool      XrWindow::submit(bool) { return false; }
bool      XrWindow::isSpaceWarpSupported() const { return false; }
void      XrWindow::setSpaceWarp(bool) {}
Tempest::Attachment* XrWindow::motionTarget(uint8_t) { return nullptr; }
Tempest::ZBuffer*    XrWindow::depthTarget(uint8_t) { return nullptr; }
Tempest::Attachment* XrWindow::eyeTarget(uint8_t) { return nullptr; }
bool      XrWindow::eyeView(uint8_t, float, Camera::EyeView&) const { return false; }
void      XrWindow::setVerticalFov(float) {}
std::string XrWindow::timingReport() { return ""; }
PadState  XrWindow::pad() const { return {}; }
bool      XrWindow::isFocused() const { return false; }
bool      XrWindow::isVisible() const { return false; }

#endif
