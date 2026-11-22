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
#include "graphics/inventoryrenderer.h"
#include "graphics/mesh/protomesh.h"
#include "world/objects/item.h"

#include <Tempest/VulkanInterop>
#include <Tempest/StorageBuffer>
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
#include <atomic>
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
static XrQuaternionf mul(const XrQuaternionf& a, const XrQuaternionf& b) {
  return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
          a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
          a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
          a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
  }
static XrQuaternionf aroundY(float angle) { return {0.f, std::sin(angle*0.5f), 0.f, std::cos(angle*0.5f)}; }
static XrQuaternionf aroundX(float angle) { return {std::sin(angle*0.5f), 0.f, 0.f, std::cos(angle*0.5f)}; }

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

// Minimum signed distance in metres from the rectangular aperture to each native
// FOV side plane (left, right, down, up). All four margins >= 0 means fully inside.
static std::array<float,4> windowFovMargins(const XrView& view, const XrPosef& window, float w, float h) {
  const auto& f = view.fov;
  const V3 normals[] = {{std::cos(f.angleLeft), 0, std::sin(f.angleLeft)},
                        {-std::cos(f.angleRight), 0, -std::sin(f.angleRight)},
                        {0, std::cos(f.angleDown), std::sin(f.angleDown)},
                        {0, -std::cos(f.angleUp), -std::sin(f.angleUp)}};
  const V3 eye = toLocal(window,toV3(view.pose.position));
  std::array<float,4> margins;
  for(size_t i=0; i<margins.size(); ++i) {
    const V3 n = rotate(conjugate(window.orientation),rotate(view.pose.orientation,normals[i]));
    // In window space the corner furthest outside minimizes the dot product with n.
    margins[i] = -dot(eye,n) - 0.5f*(w*std::abs(n.x) + h*std::abs(n.y));
    }
  return margins;
  }

// Parts of the window frame a hand can grab
enum class Grip : uint8_t { None, Handle, CornerLeft, CornerRight, Environment, Dim, Aspect, Curve, Hands, Edges, Inventory3D, SurfaceLight, ItemHandle, VrMode };

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
  // Runtimes without XR_FB_passthrough (SteamVR on Steam Frame, Android XR) may blend the room in
  // behind the layers instead: XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
  bool                     blendRoom = false;
  XrEnvironmentBlendMode   opaqueBlend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  std::string              blendModes, boundProfiles;
  bool                     hasDepth = false, hasAlphaBlend = false;
  bool                     lightEnabled = false, probePending = true, depthReady = false;
  bool                     surfaceCanProbe = false;
  bool                     permissionAsked = false, permissionGranted = false;
  bool                     permissionHintShown = false;
  bool                     traceSurface = CommandLine::inst().debugShow("surfacelight");
  bool                     traceFrameTiming = CommandLine::inst().debugShow("frametiming");
  bool                     traceWindowTracking = CommandLine::inst().debugShow("windowtracking");
  int                      tracedSurfaceSetting = -1;
  unsigned                 tracedProbeGate = ~0u;
  XrTime                   nextPermissionCheck = 0;
  int64_t                  eyeColorFormat = VK_FORMAT_UNDEFINED;
  EnvironmentDepth         environmentDepth;
  SurfaceLight             surfaceLight;
  void invalidateSurface();
  void updateSurface();
  bool                     hasHandInteraction = false;
  bool                     hasFrameController = false;
  bool                     hasMicrogestures   = false;
  bool                     hasPerfSettings    = false;
  bool                     hasSpaceWarp       = false;
  bool                     hasWindowFilter    = false;
  float                    refreshRate = 0.f;
  PFN_xrGetDisplayRefreshRateFB xrGetRate = nullptr;
  PFN_xrEnumerateDisplayRefreshRatesFB xrEnumRates = nullptr;
  PFN_xrRequestDisplayRefreshRateFB xrRequestRate = nullptr;
  std::vector<float>       supportedRates;
  int                      requestedRateChoice = -1;
  void applyRefreshRate();
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
  XrQuaternionf            headOrientation = {0,0,0,1};
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
  static constexpr float   MinWindowWidth  = 0.3375f, MaxWindowWidth  = 6.f;
  static constexpr float   MinWindowHeight = 0.3375f, MaxWindowHeight = 3.375f;
  static constexpr float   MaxWindowAspect = 32.f/9.f, MinWindowAspect = 9.f/32.f;
  static constexpr uint32_t MaxEyePixels   = 2048u*1536u;
  static constexpr uint32_t MaxSwapDim     = 3840u;
  uint32_t                 width  = 1600;
  uint32_t                 height = 900;
  uint32_t                 capacityW = 1600, capacityH = 900;
  uint32_t                 maxSwapW  = 2048, maxSwapH  = 1536;
  bool                     tightSwapchains = false;
  XrSwapchain              swapchain[2] = {};
  Tempest::Device*         device = nullptr;
  // The game renders straight into the swapchain images through these
  std::vector<Attachment>  targets[2];
  int32_t                  acquired[2] = {-1, -1};
  bool resizeSwapchains(uint32_t needW, uint32_t needH);
  static void clampWindowSize(float& w, float& h, float prevW, float prevH);

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
  // Window frame parts: grab handle (idle/hover/held), resize corners, and a single
  // consolidated chrome panel (4 buttons + dimming slider) redrawn only when dirty.
  XrSwapchain              handleSwapchain = XR_NULL_HANDLE;
  XrSwapchain              cornerSwapchain = XR_NULL_HANDLE;
  XrSwapchain              panorama = XR_NULL_HANDLE;
  XrSwapchain              chromeSwapchain = XR_NULL_HANDLE;
  bool                     chromeReady = false;
  bool                     aspectReady = false, aspectLocked = false;
  bool                     curveReady = false, curved = false;
  int                      curveLevel = 0;               // 0 flat, 1 slight, 2 strong
  bool                     sceneShown = false;
  static constexpr uint32_t ChromeW = 900, ChromeH = 135;
  static constexpr float    ChromePhysW = 0.90f, ChromePhysH = 0.135f;
  struct ChromeState {
    bool showEnv          = false;
    bool immersive        = false;
    bool aspectLocked     = false;
    bool showSlider       = false;
    bool inventory3D      = false;
    bool showSurfaceLight = false;
    bool surfaceLightOn   = false;
    bool vrMode           = false;
    int  handsMode        = 0;
    int  curveLevel       = 0;
    int  edgeStyle        = 0;
    int  dimStep          = -1;
    Grip hover            = Grip::None;
    bool operator==(const ChromeState& o) const {
      return showEnv==o.showEnv && immersive==o.immersive && aspectLocked==o.aspectLocked &&
             showSlider==o.showSlider && inventory3D==o.inventory3D &&
             showSurfaceLight==o.showSurfaceLight && surfaceLightOn==o.surfaceLightOn && vrMode==o.vrMode &&
             handsMode==o.handsMode && curveLevel==o.curveLevel && edgeStyle==o.edgeStyle &&
             dimStep==o.dimStep && hover==o.hover;
      }
    };
  ChromeState              lastChromeState;
  bool                     chromeUploaded = false;
  bool                     inventory3DEnabled = false;
  bool                     surfaceLightEnabled = false;
  // The buttons below the handle appear when the handle (or one of them) is pointed at, and
  // fade out five seconds after the last time anything there was pointed at
  bool                     hasColorScale = false;
  XrTime                   lastChromeHover = 0;
  static constexpr XrTime  ChromeLinger = 5000000000, ChromeFade = 500000000;
  float chromeReveal() const {
    if(lastChromeHover==0)
      return 0.f;
    const XrTime age = frame.predictedDisplayTime-lastChromeHover;
    if(age<=ChromeLinger-ChromeFade)
      return 1.f;
    return std::clamp(float(ChromeLinger-age)/float(ChromeFade), 0.f, 1.f);
    }
  bool chromeShown() const { return chromeReveal()>0.f; }
  // 3D inventory item preview rendered outside the window with its own grabbable handle
  XrSwapchain              itemSwapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::vector<Tempest::Attachment> itemTargets[2];
  Tempest::ZBuffer         itemDepth;
  int32_t                  itemAcquired[2] = {-1,-1};
  bool                     itemDrawn[2] = {};
  static constexpr uint32_t ItemPreviewSize = 1024;
  Tempest::RenderPipeline  itemPipeline;
  bool                     itemPreviewActive = false;
  bool                     itemOffsetLoaded = false;
  V3                       itemOffset = {0.f, -0.15f, 0.45f}; // window-local offset from window center
  V3                       grabItemOffset;
  std::string              cachedItemVisual;
  const ProtoMesh*         cachedItemMesh = nullptr;
  Tempest::Vec3            cachedItemMid;
  float                    cachedItemScale = 0.002f;
  V3   itemCenter() const { return itemOffset; }
  V3   itemHandleCenter() const { return itemCenter() + V3{0.f, -0.15f, 0.f}; }
  void loadItemOffset();
  void saveItemOffset();
  // Ghost hands and pointer rays
  bool                     hasHandTracking = false;
  PFN_xrCreateHandTrackerEXT  xrCreateHandTracker = nullptr;
  PFN_xrDestroyHandTrackerEXT xrDestroyHandTracker = nullptr;
  PFN_xrLocateHandJointsEXT   xrLocateHandJoints = nullptr;
  XrHandTrackerEXT         handTracker[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> joints[2] = {};
  XrSwapchain              handsSwapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  std::vector<Tempest::Attachment> handsTargets[2];
  int32_t                  handsAcquired[2] = {-1,-1};
  bool                     handsDrawn[2] = {};
  static constexpr uint32_t HandsSize = 1024;
  XrView                   eyeViews[2] = {{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
  struct Capsule { float a[4], b[4], tint[4]; };
  std::vector<Capsule>     handCapsules;
  std::array<Tempest::StorageBuffer,8> handsBuffer;
  Tempest::RenderPipeline  handsPipeline, handsFill;
  bool                     handsButtonReady = false;
  int                      handsMode = 0;
  float                    handsAlpha[2] = {};
  // Hero arm tracking: hand joints located against the head every frame while enabled
  bool                     armTracking = false;
  std::array<XrHandJointLocationEXT,XR_HAND_JOINT_COUNT_EXT> armJoints[2] = {};
  XrHandTarget             armTarget[2];
  XrQuaternionf            armHead = {0,0,0,1};
  bool                     armHeadValid = false;
  void updateArmTargets();
  std::atomic<int64_t>     lastGameInput{0};             // steady clock, nanoseconds
  float                    curveRadius = 0.f;            // metres: the arc's radius, centred near the viewer
  uint8_t                  recordingEye = 0;
  std::array<std::array<float,4>,2> eyeRect = {};        // per eye: left, right, bottom, top on the window plane
  Tempest::RenderPipeline  curveCoverage, curveMotion;
  static constexpr int     RailW = 160, KnobW = 24, SliderRows = 16;
  // Window controls scale with the window (1 at the default size), not below half size
  static constexpr float   DefaultWindowHeight = 0.9f;
  float chromeScale() const { return std::clamp(std::sqrt(windowWidth*windowHeight()/(1.6f*DefaultWindowHeight)), 0.5f, 1.4f); }
  float dimLeft()   const { return -0.10f*chromeScale(); } // relative to sliderCenter()
  float dimTravel() const { return 0.20f*chromeScale(); }
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
  bool                     frameWaited = false;
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
  XrAction                 aThumbTap  = XR_NULL_HANDLE;   // microgesture, hand tracking only
  std::array<Hand,2>       hands;
  PadState                 padState, releasePad;
  enum PadControl : size_t { StickLeft=PadState::Count, StickRight, TriggerLeft, TriggerRight, ControlCount };
  std::bitset<ControlCount> blockedControls;
  bool                     padInterrupted = false;
  // Game paused or a game menu open: the triggers point at the window controls instead of playing
  bool                     pointerMode = false;
  // Per hand: a controller of any make (true until the runtime says otherwise) or a tracked hand
  bool                     controllerHand[2] = {true, true};
  void updateInteractionProfiles();

  // VR mode: the game fills the view from the wearer's eyes (no window, chrome or room)
  bool                     vrMode     = false;  // chosen by the wearer (INTERNAL xrVrMode)
  bool                     vrActive   = false;  // this frame renders the VR view
  bool                     vrRefValid = false;
  float                    vrRefYaw   = 0.f;    // head yaw (radians) when the view started
  V3                       vrRefPos;            // head position then, LOCAL space
  V3                       vrEyeAnchor;         // VR eye minus the hero's head bone, game world axes, cm
  XrQuaternionf            vrBase     = {0,0,0,1}; // head yaw and pitch handed to the camera
  V3                       vrBasePos;           // head position handed to the camera
  XrTime                   vrLastTap = 0, vrToggleAfter = 0, vrGripSince = 0;
  bool                     vrGripLatched = false;
  mutable uint32_t         vrRecommended[2] = {};
  static constexpr float   VrNear = 0.05f;      // metres
  // The interface in VR: a head-locked panel this far ahead (metres), its half width and its
  // centre's drop below the gaze as tangents (about 45 degrees wide, 5 degrees low)
  static constexpr float   VrPanelDistance = 1.5f, VrPanelHalf = 0.42f, VrPanelDrop = 0.08f;
  static constexpr uint32_t VrEyePixels = 1536u*1536u;
  void updateVrToggles();
  void toggleVrMode(const char* source);

  ~Impl();

  bool createInstance();
  void createSession();
  void createSwapchains();
  void createPassthrough();
  void destroyPassthrough();
  void passthroughChanged(XrPassthroughStateChangedFlagsFB flags);
  // XR_FB_passthrough stays preferred; alpha blending stands in only while there is no passthrough layer
  bool roomAvailable() const { return passthroughLayer!=XR_NULL_HANDLE ? !passthroughUnavailable : blendRoom; }
  bool blendsRoom() const { return passthroughLayer==XR_NULL_HANDLE && blendRoom; }
  void queryBlendModes();
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
  void updateChromePanel(bool showEnv, bool showSlider, bool showSurfaceLight, Grip hover);
  V3   buttonCenter() const { return handleCenter() + V3{0,-0.10f*chromeScale(),0}; }
  V3   chromeCenter() const { return buttonCenter() + V3{0,-0.0325f*chromeScale(),0}; }
  // The row below the handle, centred: hands, curve, edges, aspect lock, 3D inventory, surface light,
  // sky/passthrough, VR mode; the dimming slider sits below the row
  V3   handsCenter() const        { return buttonCenter() + V3{-0.385f*chromeScale(),0,0}; }
  V3   curveCenter() const        { return buttonCenter() + V3{-0.275f*chromeScale(),0,0}; }
  V3   edgesCenter() const        { return buttonCenter() + V3{-0.165f*chromeScale(),0,0}; }
  V3   aspectCenter() const       { return buttonCenter() + V3{-0.055f*chromeScale(),0,0}; }
  V3   inventory3DCenter() const  { return buttonCenter() + V3{ 0.055f*chromeScale(),0,0}; }
  V3   surfaceLightCenter() const { return buttonCenter() + V3{ 0.165f*chromeScale(),0,0}; }
  V3   environmentCenter() const  { return buttonCenter() + V3{ 0.275f*chromeScale(),0,0}; }
  V3   vrModeCenter() const       { return buttonCenter() + V3{ 0.385f*chromeScale(),0,0}; }
  V3   sliderCenter() const       { return buttonCenter() + V3{0,-0.07f*chromeScale(),0}; }
  bool isCurved() const { return curved && curveRadius>0.f && sceneShown && !vrActive; }
  // The arc is centred on the viewer at the moment the window is placed or curved
  // Slight: the arc centred on the viewer. Strong: a tighter arc (the viewer stays inside it).
  void updateCurveRadius() {
    const float d = std::clamp(length(headPos-toV3(windowPose.position)),0.8f,6.f);
    curveRadius = curveLevel>=2 ? 0.7f*d : d;
    }
  // Keep the curved arc within ~100 degrees total so wide windows (up to 32:9) curve
  // around the viewer like a wrap-around monitor without approaching or crossing the eyes.
  float effectiveCurveRadius() const {
    return isCurved() ? std::max(curveRadius, windowWidth*0.5f/0.90f) : 0.f;
    }
  // How far the window's side edges come towards the viewer
  float curveSag() const {
    if(!isCurved())
      return 0.f;
    const float R = effectiveCurveRadius();
    return R*(1.f-std::cos(windowWidth*0.5f/R));
    }
  // Window space <-> unrolled arc space (x: arc length, y: height, z: distance in front of the arc).
  // Hit tests and resizing work in unrolled space, where the curved window is a flat rectangle.
  V3   unroll(const V3& p) const {
    if(!isCurved())
      return p;
    const float R = effectiveCurveRadius();
    return V3{R*std::atan2(p.x,R-p.z), p.y, R-std::hypot(p.x,R-p.z)};
    }
  V3   roll(const V3& u) const {
    if(!isCurved())
      return u;
    const float R = effectiveCurveRadius(), a = u.x/R, r = R-u.z;
    return V3{r*std::sin(a), u.y, R-r*std::cos(a)};
    }
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
      handsTargets[eye].clear();
      }
    for(auto& b:handsBuffer)
      b = Tempest::StorageBuffer();
    handsPipeline = Tempest::RenderPipeline();
    handsFill     = Tempest::RenderPipeline();
    itemPipeline  = Tempest::RenderPipeline();
    itemDepth     = Tempest::ZBuffer();
    for(auto& t:itemTargets)
      t.clear();
    }
  for(auto& t:handTracker)
    if(t!=XR_NULL_HANDLE && xrDestroyHandTracker)
      xrDestroyHandTracker(t);
  for(auto& sc:itemSwapchain)
    if(sc!=XR_NULL_HANDLE)
      xrDestroySwapchain(sc);
  for(auto& sc:handsSwapchain)
    if(sc!=XR_NULL_HANDLE)
      xrDestroySwapchain(sc);
  environmentDepth.reset();
  surfaceLight.reset();
  destroyPassthrough();
  if(panorama!=XR_NULL_HANDLE)
    xrDestroySwapchain(panorama);
  if(chromeSwapchain!=XR_NULL_HANDLE)
    xrDestroySwapchain(chromeSwapchain);
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
  // Steam Frame controllers; without it SteamVR emulates Touch controllers
  hasFrameController = has("XR_VALVE_frame_controller_interaction");
  if(hasFrameController)
    ext.push_back("XR_VALVE_frame_controller_interaction");
  // Double thumb tap toggles VR mode (its paths extend the hand interaction profile)
  hasMicrogestures = hasHandInteraction && has(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME);
  if(hasMicrogestures)
    ext.push_back(XR_META_HAND_TRACKING_MICROGESTURES_EXTENSION_NAME);
  hasColorScale = has(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
  if(hasColorScale)
    ext.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
  hasHandTracking = has(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
  if(hasHandTracking)
    ext.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);

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
  if(hasHandTracking) {
    fn("xrCreateHandTrackerEXT",xrCreateHandTracker);
    fn("xrDestroyHandTrackerEXT",xrDestroyHandTracker);
    fn("xrLocateHandJointsEXT",xrLocateHandJoints);
    }
  if(hasRefreshRate) {
    fn("xrGetDisplayRefreshRateFB",xrGetRate);
    fn("xrEnumerateDisplayRefreshRatesFB",xrEnumRates);
    fn("xrRequestDisplayRefreshRateFB",xrRequestRate);
    }
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
  Impl::clampWindowSize(impl->windowWidth, impl->physicalHeight, impl->windowWidth, impl->physicalHeight);
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
  queryBlendModes();
  createPassthrough();
  createActions();
  if(xrCreateHandTracker && xrLocateHandJoints) {
    for(int i=0; i<2; ++i) {
      XrHandTrackerCreateInfoEXT ci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
      ci.hand         = i==0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
      ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
      if(!ok(xrCreateHandTracker(session,&ci,&handTracker[i]),"hand tracker"))
        handTracker[i] = XR_NULL_HANDLE;
      }
    }
  // One line for remote debugging on other headsets
  Log::i("OpenXR: room=", passthroughLayer!=XR_NULL_HANDLE ? "XR_FB_passthrough" : (blendRoom ? "alpha blend" : "none"),
         " blend modes=", blendModes.empty() ? std::string("?") : blendModes,
         " profiles=", boundProfiles.empty() ? std::string("none") : boundProfiles,
         " hand tracking=", handTracker[0]!=XR_NULL_HANDLE ? "yes" : "no");
  }

void XrWindow::Impl::queryBlendModes() {
  uint32_t count = 0;
  if(!ok(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr),
         "count blend modes") || count==0)
    return;
  std::vector<XrEnvironmentBlendMode> modes(count);
  if(!ok(xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, modes.data()),
         "enumerate blend modes"))
    return;
  modes.resize(count);
  bool opaque = false;
  for(auto m:modes) {
    opaque    |= m==XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    blendRoom |= m==XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    if(!blendModes.empty())
      blendModes += ',';
    blendModes += m==XR_ENVIRONMENT_BLEND_MODE_OPAQUE      ? "opaque" :
                  m==XR_ENVIRONMENT_BLEND_MODE_ADDITIVE    ? "additive" :
                  m==XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? "alpha" : "other";
    }
  // The runtime's preferred mode comes first
  if(!opaque && !modes.empty())
    opaqueBlend = modes[0];
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
    maxSwapW  = std::min(MaxSwapDim, props.graphicsProperties.maxSwapchainImageWidth);
    maxSwapH  = std::min(MaxSwapDim, props.graphicsProperties.maxSwapchainImageHeight);
    capacityW = std::min(2048u, maxSwapW);
    capacityH = std::min(1536u, maxSwapH);
    }
  for(const auto& value:CommandLine::inst().settingsOverrides()) {
    if(value.section=="BENCHMARK" && value.key=="xrTightSwapchains" && value.value=="1") {
      tightSwapchains = true;
      capacityW = std::min(capacityW,width);
      capacityH = std::min(capacityH,height);
      maxSwapW  = capacityW;
      maxSwapH  = capacityH;
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

  {
    XrSwapchainCreateInfo hi = info;
    hi.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                    XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    hi.width  = HandsSize;
    hi.height = HandsSize;
    for(int eye=0; eye<2; ++eye) {
      handsSwapchain[eye] = makeSwapchain(session,hi,"hands swapchain");
      if(handsSwapchain[eye]==XR_NULL_HANDLE)
        break;
      for(auto& im:swapchainImages(handsSwapchain[eye]))
        handsTargets[eye].push_back(vulkanBorrowColorAttachment(*device,im.image,HandsSize,HandsSize));
      }
    hi.width  = ItemPreviewSize;
    hi.height = ItemPreviewSize;
    for(int eye=0; eye<2; ++eye) {
      itemSwapchain[eye] = makeSwapchain(session,hi,"inventory item preview swapchain");
      if(itemSwapchain[eye]==XR_NULL_HANDLE)
        break;
      for(auto& im:swapchainImages(itemSwapchain[eye]))
        itemTargets[eye].push_back(vulkanBorrowColorAttachment(*device,im.image,ItemPreviewSize,ItemPreviewSize));
      }
  }

  // Single dynamic swapchain for the consolidated chrome toolbar (4 buttons + dimming slider)
  {
    XrSwapchainCreateInfo ci = info;
    ci.createFlags = 0;
    ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.width       = ChromeW;
    ci.height      = ChromeH;
    ci.arraySize   = 1;
    chromeSwapchain = makeSwapchain(session, ci, "chrome panel swapchain");
  }

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

void XrWindow::Impl::clampWindowSize(float& w, float& h, float prevW, float prevH) {
  w = std::clamp(w, MinWindowWidth, MaxWindowWidth);
  h = std::clamp(h, MinWindowHeight, MaxWindowHeight);
  if(w>h*MaxWindowAspect) {
    if(h<prevH && w<=prevW) {
      h = std::min(MaxWindowHeight, w/MaxWindowAspect);
      w = h*MaxWindowAspect;
      } else if(w>prevW && h>=prevH) {
      w = h*MaxWindowAspect;
      } else {
      const float area = w*h;
      h = std::clamp(std::sqrt(area/MaxWindowAspect), MinWindowHeight, MaxWindowWidth/MaxWindowAspect);
      w = h*MaxWindowAspect;
      }
    } else if(w<h*MinWindowAspect) {
    if(w<prevW && h<=prevH) {
      w = std::min(MaxWindowWidth, h*MinWindowAspect);
      h = w/MinWindowAspect;
      } else if(h>prevH && w>=prevW) {
      h = w/MinWindowAspect;
      } else {
      const float area = w*h;
      w = std::clamp(std::sqrt(area*MinWindowAspect), MinWindowWidth, MaxWindowHeight*MinWindowAspect);
      h = w/MinWindowAspect;
      }
    }
  w = std::clamp(w, MinWindowWidth, MaxWindowWidth);
  h = std::clamp(h, MinWindowHeight, MaxWindowHeight);
  }

bool XrWindow::Impl::resizeSwapchains(uint32_t needW, uint32_t needH) {
  if(needW<=capacityW && needH<=capacityH)
    return true;
  if(tightSwapchains || device==nullptr || session==XR_NULL_HANDLE ||
     acquired[0]>=0 || acquired[1]>=0 || mvAcquired[0]>=0 || mvAcquired[1]>=0 ||
     depthAcquired[0]>=0 || depthAcquired[1]>=0)
    return false;

  auto roundUp64 = [](uint32_t v) { return ((v+63u)/64u)*64u; };
  uint32_t newW = std::min(maxSwapW, std::max(needW, roundUp64(uint32_t(std::ceil(float(needW)*1.12f)))));
  uint32_t newH = std::min(maxSwapH, std::max(needH, roundUp64(uint32_t(std::ceil(float(needH)*1.12f)))));
  // Keep swapchains divisible by 4 so quarter-resolution SpaceWarp buffers align cleanly.
  newW = std::max(4u, (newW/4u)*4u);
  newH = std::max(4u, (newH/4u)*4u);
  if(newW<needW || newH<needH)
    return false;

  device->waitIdle();

  XrSwapchainCreateInfo info = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
  if(CommandLine::inst().xrCaptureInterval()>0)
    info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
  info.format      = eyeColorFormat;
  info.sampleCount = 1;
  info.faceCount   = 1;
  info.arraySize   = 1;
  info.mipCount    = 1;
  info.width       = newW;
  info.height      = newH;

  XrSwapchain newColor[2] = {}, newMv[2] = {}, newDepth[2] = {};
  std::vector<Attachment> newTargets[2], newMvTargets[2];
  std::vector<ZBuffer>    newDepthTargets[2];
  const bool rebuildWarp = hasSpaceWarp && !fullMotionExtent && mvSwapchain[0]!=XR_NULL_HANDLE && depthSwapchain[0]!=XR_NULL_HANDLE;
  const auto& cli = CommandLine::inst();
  const XrSwapchainUsageFlags readback = cli.xrCaptureBurst()>0 && cli.debugShow("warpraw") ?
                                         XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0;

  auto cleanup = [&](XrSwapchain (&c)[2], std::vector<Attachment> (&t)[2],
                     XrSwapchain (&m)[2], std::vector<Attachment> (&mt)[2],
                     XrSwapchain (&d)[2], std::vector<ZBuffer> (&dt)[2]) {
    for(int eye=0; eye<2; ++eye) {
      t[eye].clear();
      mt[eye].clear();
      dt[eye].clear();
      for(auto* ch : {&c[eye], &m[eye], &d[eye]})
        if(*ch!=XR_NULL_HANDLE)
          xrDestroySwapchain(std::exchange(*ch, XR_NULL_HANDLE));
      }
    };

  bool okAll = false;
  try {
    okAll = true;
    for(int eye=0; eye<2 && okAll; ++eye) {
      newColor[eye] = makeSwapchain(session, info, "resize window swapchain");
      if(newColor[eye]==XR_NULL_HANDLE) { okAll = false; break; }
      auto imgs = swapchainImages(newColor[eye]);
      if(imgs.empty()) { okAll = false; break; }
      for(auto& im:imgs)
        newTargets[eye].push_back(vulkanBorrowColorAttachment(*device, im.image, newW, newH));

      if(rebuildWarp) {
        XrSwapchainCreateInfo wi = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        wi.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | readback;
        wi.format      = VK_FORMAT_R16G16B16A16_SFLOAT;
        wi.sampleCount = wi.faceCount = wi.arraySize = wi.mipCount = 1;
        wi.width       = std::max(newW/4u, 16u);
        wi.height      = std::max(newH/4u, 16u);
        newMv[eye] = makeSwapchain(session, wi, "resize motion vector swapchain");
        if(newMv[eye]==XR_NULL_HANDLE) { okAll = false; break; }
        wi.usageFlags  = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | readback;
        wi.format      = VK_FORMAT_D32_SFLOAT;
        newDepth[eye] = makeSwapchain(session, wi, "resize depth swapchain");
        if(newDepth[eye]==XR_NULL_HANDLE) { okAll = false; break; }
        for(auto& im:swapchainImages(newMv[eye]))
          newMvTargets[eye].push_back(vulkanBorrowColorAttachment(*device, im.image, wi.width, wi.height, TextureFormat::RGBA16F));
        for(auto& im:swapchainImages(newDepth[eye]))
          newDepthTargets[eye].push_back(vulkanBorrowDepthAttachment(*device, im.image, wi.width, wi.height, TextureFormat::Depth32F));
        if(newMvTargets[eye].empty() || newDepthTargets[eye].empty()) { okAll = false; break; }
        }
      }
    }
  catch(const std::bad_alloc&) { okAll = false; }
  catch(const std::system_error& e) {
    if(e.code()!=GraphicsErrc::OutOfVideoMemory && e.code()!=GraphicsErrc::OutOfHostMemory)
      throw;
    okAll = false;
    }

  if(!okAll) {
    cleanup(newColor, newTargets, newMv, newMvTargets, newDepth, newDepthTargets);
    Log::e("OpenXR: could not resize eye swapchains to ", newW, "x", newH);
    return false;
    }

  XrSwapchain oldColor[2] = {}, oldMv[2] = {}, oldDepth[2] = {};
  std::vector<Attachment> oldTargets[2], oldMvTargets[2];
  std::vector<ZBuffer>    oldDepthTargets[2];
  for(int eye=0; eye<2; ++eye) {
    oldColor[eye] = std::exchange(swapchain[eye], newColor[eye]);
    oldTargets[eye] = std::exchange(targets[eye], std::move(newTargets[eye]));
    if(rebuildWarp) {
      oldMv[eye] = std::exchange(mvSwapchain[eye], newMv[eye]);
      oldMvTargets[eye] = std::exchange(mvTargets[eye], std::move(newMvTargets[eye]));
      oldDepth[eye] = std::exchange(depthSwapchain[eye], newDepth[eye]);
      oldDepthTargets[eye] = std::exchange(depthTargets[eye], std::move(newDepthTargets[eye]));
      }
    }
  cleanup(oldColor, oldTargets, oldMv, oldMvTargets, oldDepth, oldDepthTargets);
  capacityW = newW;
  capacityH = newH;
  skipExtrapolation = true;
  Log::i("OpenXR: resized eye swapchains to ", capacityW, "x", capacityH);
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
  // Dimming applies to passthrough only: hidden in VR mode
  // Darkens the passthrough room, or the landscape in VR mode; shown with the buttons
  // Alpha-blended room: no dimming (XR_FB_passthrough styles only)
  const bool canDim = immersive ? hasColorScale : (dimStyleAvailable && !blendsRoom() && roomAvailable());
  if(!sliderReady || !canDim || !chromeShown())
    return -1;
  return SliderRows-1;
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
  if(hasMicrogestures)
    make(aThumbTap, "thumb_tap",    "Thumb tap",        XR_ACTION_TYPE_BOOLEAN_INPUT,  true);

  auto suggest = [&](const char* profile, std::initializer_list<std::pair<XrAction,const char*>> list) -> bool {
    const auto profilePath = path(profile);
    if(profilePath==XR_NULL_PATH)
      return false;
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
      return false;
    XrInteractionProfileSuggestedBinding s = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile     = profilePath;
    s.countSuggestedBindings = uint32_t(b.size());
    s.suggestedBindings      = b.data();
    // A rejected profile leaves the others usable
    if(!ok(xrSuggestInteractionProfileBindings(instance, &s), profile))
      return false;
    if(!boundProfiles.empty())
      boundProfiles += ',';
    boundProfiles += std::string_view(profile).substr(std::strlen("/interaction_profiles/"));
    return true;
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
    auto handProfile = [&](XrAction thumbTap) {
      return suggest("/interaction_profiles/ext/hand_interaction_ext", {
        {aAim,  "/user/hand/left/input/aim/pose"},
        {aAim,  "/user/hand/right/input/aim/pose"},
        {aNear, "/user/hand/left/input/pinch_ext/pose"},
        {aNear, "/user/hand/right/input/pinch_ext/pose"},
        {aGrab, "/user/hand/left/input/pinch_ext/value"},
        {aGrab, "/user/hand/right/input/pinch_ext/value"},
        // Skipped (null action) unless XR_META_hand_tracking_microgestures is enabled
        {thumbTap, "/user/hand/left/input/tap_thumb_meta/click"},
        {thumbTap, "/user/hand/right/input/tap_thumb_meta/click"},
        });
      };
    // A runtime that rejects the microgesture paths must not cost the hands their pinch and aim
    if(!handProfile(aThumbTap) && aThumbTap!=XR_NULL_HANDLE) {
      Log::e("OpenXR: hand profile rejected with microgestures, retrying without the thumb tap");
      handProfile(XR_NULL_HANDLE);
      }
    }
  // Other runtimes (SteamVR, generic OpenXR); component paths per the OpenXR spec.
  // The simple profile only where hands have no profile of their own: a runtime could map them to it.
  if(!hasHandInteraction) {
    suggest("/interaction_profiles/khr/simple_controller", {
      {aAim,     "/user/hand/left/input/aim/pose"},
      {aAim,     "/user/hand/right/input/aim/pose"},
      {aNear,    "/user/hand/left/input/grip/pose"},
      {aNear,    "/user/hand/right/input/grip/pose"},
      {aTrigger, "/user/hand/left/input/select/click"},
      {aTrigger, "/user/hand/right/input/select/click"},
      {aMenu,    "/user/hand/left/input/menu/click"},
      {aMenu,    "/user/hand/right/input/menu/click"},
      });
    }
  // Index has no menu button: the left system button (the runtime may keep it for itself)
  suggest("/interaction_profiles/valve/index_controller", {
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
    {aX,          "/user/hand/left/input/a/click"},
    {aY,          "/user/hand/left/input/b/click"},
    {aMenu,       "/user/hand/left/input/system/click"},
    });
  if(hasFrameController) {
    // Steam Frame: all four face buttons are on the right controller, View (left) is Touch's menu
    suggest("/interaction_profiles/valve/frame_controller_valve", {
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
      {aX,          "/user/hand/right/input/x/click"},
      {aY,          "/user/hand/right/input/y/click"},
      {aMenu,       "/user/hand/left/input/view/click"},
      {aMenu,       "/user/hand/right/input/menu/click"},
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
  if(begin && xrEnumRates) {
    uint32_t count = 0;
    supportedRates.clear();
    if(XR_SUCCEEDED(xrEnumRates(session,0,&count,nullptr)) && count>0) {
      supportedRates.resize(count);
      if(XR_FAILED(xrEnumRates(session,count,&count,supportedRates.data())))
        supportedRates.clear();
      supportedRates.resize(std::min<size_t>(count,supportedRates.size()));
      }
    std::string list;
    for(float r:supportedRates)
      list += std::to_string(int(std::lround(r))) + " ";
    Log::i("OpenXR: display refresh rates ", list);
    requestedRateChoice = -1;
    }
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
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
        if(reinterpret_cast<const XrEventDataInteractionProfileChanged&>(ev).session!=session)
          break;
        // Controllers picked up or put down: whatever was held or dragged belongs to the old device
        updateInteractionProfiles();
        cancelInput();
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

void XrWindow::Impl::updateInteractionProfiles() {
  XrPath handProfile = XR_NULL_PATH;
  if(XR_FAILED(xrStringToPath(instance, "/interaction_profiles/ext/hand_interaction_ext", &handProfile)))
    handProfile = XR_NULL_PATH;
  for(size_t i=0; i<hands.size(); ++i) {
    XrInteractionProfileState st = {XR_TYPE_INTERACTION_PROFILE_STATE};
    if(hands[i].path==XR_NULL_PATH || XR_FAILED(xrGetCurrentInteractionProfile(session, hands[i].path, &st)))
      continue;
    // Any profile but tracked hands is a controller with a trigger, whatever its make
    controllerHand[i] = st.interactionProfile==XR_NULL_PATH || st.interactionProfile!=handProfile;
    char name[XR_MAX_PATH_LENGTH] = "none";
    uint32_t len = 0;
    if(st.interactionProfile!=XR_NULL_PATH &&
       XR_FAILED(xrPathToString(instance, st.interactionProfile, sizeof(name), &len, name)))
      std::snprintf(name, sizeof(name), "unknown");
    Log::i("OpenXR: ", i==0 ? "left" : "right", " hand profile ", name);
    }
  }

void XrWindow::Impl::placeWindow() {
  const auto due = [this](XrTime t) { return t<=frame.predictedDisplayTime; };
  if(std::erase_if(recenterTimes,due)!=0) {
    placed = false;
    vrRefValid = false; // a recenter also sets where VR looks forward
    if(probePlaced) {
      probePlaced = false;
      probeStableSince = 0;
      Log::i("XR bench placement reset by LOCAL space change");
      }
    cancelInput();
    }
  skipExtrapolation |= !placed;
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
  headOrientation = head.pose.orientation;
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
  // A little beyond the sweet spot (where the window covers the game's field of view), so the
  // whole window fits in view with room to spare
  const float distance = std::clamp(portalDistance(verticalFov)*1.1f, 1.0f, 1.4f);
  const V3    pos      = headPos + fwd*std::sqrt(distance*distance-0.1f*0.1f) + V3{0,-0.1f,0};
  windowPose.position    = toXr(pos);
  windowPose.orientation = facing(headPos - pos);
  placed = true;
  if(curved)
    updateCurveRadius();
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

  V3 point = environmentCenter(); // the sky/passthrough toggle the log lines above report
  bool ray = false;
  if(f<320) {
    hand.grab = (f>=108 && f<116) || (f>=292 && f<300) ? 1.f : 0.f;
    } else if(f<530) {
    // Aim and trigger. Start at the light end and drag continuously.
    point = sliderCenter();
    point.x += dimLeft()+dimTravel()*0.85f*std::clamp((float(f)-330.f)/94.f,0.f,1.f);
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
  updateVrToggles();

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
  const bool canManipulate = frame.shouldRender && placed && headValid && !vrActive; // no window in VR
  const int64_t steadyNow = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
  const bool gameInputRecent = steadyNow-lastGameInput.load() < 1000000000;
  // Paused, the buttons below the window stay out for the pointer (they fade as usual afterwards)
  if(pointerMode && canManipulate)
    lastChromeHover = frame.predictedDisplayTime;
  const V3 wNormal = rotate(windowPose.orientation, V3{0,0,1});
  auto rayHit = [&](const XrPosef& aim) -> std::optional<V3> {
    const V3 origin = toV3(aim.position);
    const V3 dir = rotate(aim.orientation,V3{0,0,-1});
    if(isCurved()) {
      // The window's cylinder: vertical axis at distance R in front of the window centre
      const V3 o = toLocal(windowPose, origin);
      const V3 d = rotate(conjugate(windowPose.orientation), dir);
      const float R = effectiveCurveRadius();
      const float a = d.x*d.x + d.z*d.z;
      const float b = 2.f*(o.x*d.x - (R-o.z)*d.z);
      const float c = o.x*o.x + (R-o.z)*(R-o.z) - R*R;
      const float disc = b*b - 4.f*a*c;
      if(a<1e-6f || disc<0.f)
        return {};
      const float sq = std::sqrt(disc);
      for(const float t : {(-b+sq)/(2.f*a), (-b-sq)/(2.f*a)}) {
        const V3 p = o + d*t;
        if(t>0.f && R-p.z>0.f)
          return toWorld(windowPose, p);
        }
      return {};
      }
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
    // Menu navigation on the pad does not hide the controls from the pointer
    if(gameInputRecent && !pointerMode && grabbedBy!=int(i))
      continue;

    // Direct touch first: the pinch point (or controller grip) close to a frame part
    if(h.nearValid) {
      if(itemPreviewActive && handleReady) {
        const V3 nl  = toLocal(windowPose, toV3(h.nearPose.position));
        const V3 ihc = itemHandleCenter();
        if(std::abs(nl.x-ihc.x)<0.09f && std::abs(nl.y-ihc.y)<0.045f && std::abs(nl.z-ihc.z)<0.07f) {
          h.hover     = Grip::ItemHandle;
          h.hoverNear = true;
          h.hit       = toV3(h.nearPose.position);
          continue;
          }
        }
      const V3 n = unroll(toLocal(windowPose, toV3(h.nearPose.position)));
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
    // Then pointing: the aim ray against the 3D item handle (in front) or the window plane
    if(!h.aimValid)
      continue;
    if(itemPreviewActive && handleReady) {
      const V3 ihc = itemHandleCenter();
      const V3 o = toLocal(windowPose, toV3(h.aim.position));
      const V3 d = rotate(conjugate(windowPose.orientation), rotate(h.aim.orientation, V3{0,0,-1}));
      if(std::abs(d.z)>1e-5f) {
        const float t = (ihc.z - o.z)/d.z;
        if(t>0.02f && t<10.f) {
          const V3 l = o + d*t;
          if(std::abs(l.x-ihc.x)<0.09f && std::abs(l.y-ihc.y)<0.045f) {
            h.hover = Grip::ItemHandle;
            h.hit   = toWorld(windowPose, l);
            continue;
            }
          }
        }
      }
    const auto p = rayHit(h.aim);
    if(!p)
      continue;
    const V3 l = unroll(toLocal(windowPose, *p));
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
    // In pointer mode the trigger works every control; otherwise it only drags the dimming slider
    const bool startTrigger = (h.hover==Grip::Dim || (pointerMode && h.hover!=Grip::None)) && triggerDown;
    auto moveDim = [&](const V3& worldPoint) {
      const float x = toLocal(windowPose,worldPoint).x;
      const float level = std::clamp((x-sliderCenter().x-dimLeft())/dimTravel(),0.f,1.f);
      if(level==passthroughDim)
        return;
      if(immersive)
        passthroughDim = level, dimDirty = true; // the landscape takes it in submit()
      else if(applyDim(level))
        dimDirty = true;
      };
    if(canManipulate && (h.hover==Grip::Dim || h.hover==Grip::Environment))
      lastDimHover = frame.predictedDisplayTime;
    if(grabbedBy==int(i)) {
      const bool tracked = grabNear ? h.nearValid : h.aimValid;
      const auto& action = grabTrigger ? h.trigger : h.grab;
      // A trigger drag ends with pointer mode: the trigger then belongs to the game again
      if(!action || *action<0.35f || !tracked || (grabTrigger && !pointerMode && grabbed!=Grip::Dim) ||
         ((grabbed==Grip::Environment || grabbed==Grip::Dim) && !roomAvailable()) ||
         (grabbed==Grip::Dim && !(immersive ? hasColorScale : dimStyleAvailable))) {
        saveDim();
        if(grabbed==Grip::ItemHandle)
          saveItemOffset();
        if(grabbed==Grip::Handle && curved) {
          updateCurveRadius();
          skipExtrapolation = true;
          }
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
      if(!canManipulate || grabbed==Grip::Environment || grabbed==Grip::Aspect || grabbed==Grip::Curve ||
         grabbed==Grip::Hands || grabbed==Grip::Edges || grabbed==Grip::Inventory3D || grabbed==Grip::SurfaceLight ||
         grabbed==Grip::VrMode) {
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
        skipExtrapolation = true;
        } else if(grabbed==Grip::ItemHandle) {
        const V3 delta = toLocal(windowPose, point) - grabWindow;
        itemOffset = V3{std::clamp(grabItemOffset.x + delta.x, -1.5f, 1.5f),
                        std::clamp(grabItemOffset.y + delta.y, -1.0f, 1.2f),
                        std::clamp(grabItemOffset.z + delta.z,  0.12f, 1.5f)};
        } else {
        // A corner follows the hand across the window plane; the centre stays put
        const V3    l  = unroll(toLocal(windowPose, point));
        const V3    g0 = unroll(grabWindow);
        const float dx = (l.x-g0.x)*(g0.x<0.f ? -2.f : 2.f);
        const float dy = (l.y-g0.y)*(g0.y<0.f ? -2.f : 2.f);
        const float prevW = windowWidth, prevH = physicalHeight;
        if(aspectLocked) {
          // Locked: both edges follow the average stretch, so the shape stays
          const float k = std::clamp(((grabExtent.x+dx)/grabExtent.x + (grabExtent.y+dy)/grabExtent.y)*0.5f,
                                     std::max(MinWindowWidth/grabExtent.x, MinWindowHeight/grabExtent.y),
                                     std::min(MaxWindowWidth/grabExtent.x, MaxWindowHeight/grabExtent.y));
          windowWidth    = grabExtent.x*k;
          physicalHeight = grabExtent.y*k;
          } else {
          windowWidth    = grabExtent.x+dx;
          physicalHeight = grabExtent.y+dy;
          clampWindowSize(windowWidth, physicalHeight, prevW, prevH);
          }
        skipExtrapolation = true;
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
        } else if(grabbed==Grip::Hands) {
        handsMode = (handsMode+1)%3;
        Gothic::settingsSetI("INTERNAL", "xrHands", handsMode);
        Gothic::flushSettings();
        } else if(grabbed==Grip::Curve) {
        curveLevel = (curveLevel+1)%3;
        curved = curveLevel>0;
        if(curved)
          updateCurveRadius();
        skipExtrapolation = true;
        Gothic::settingsSetI("INTERNAL", "xrCurved", curveLevel);
        Gothic::flushSettings();
        } else if(grabbed==Grip::Aspect) {
        aspectLocked = !aspectLocked;
        Gothic::settingsSetI("INTERNAL", "xrAspectLock", aspectLocked ? 1 : 0);
        Gothic::flushSettings();
        } else if(grabbed==Grip::Edges) {
        edgeStyle = (edgeStyle+1)%3;
        skipExtrapolation = true;
        Gothic::settingsSetI("INTERNAL", "xrWindowEdges", edgeStyle);
        Gothic::flushSettings();
        } else if(grabbed==Grip::Inventory3D) {
        inventory3DEnabled = !inventory3DEnabled;
        Gothic::settingsSetI("INTERNAL", "xrInventory3D", inventory3DEnabled ? 1 : 0);
        Gothic::flushSettings();
        } else if(grabbed==Grip::SurfaceLight) {
        surfaceLightEnabled = !surfaceLightEnabled;
        Gothic::settingsSetI("INTERNAL", "xrSurfaceLight", surfaceLightEnabled ? 1 : 0);
        Gothic::flushSettings();
        } else if(grabbed==Grip::Environment) {
        if(setEnvironment(!immersive)) {
          restoreSky = false;
          Gothic::settingsSetI("INTERNAL", "xrEnvironment", immersive ? 1 : 0);
          Gothic::flushSettings();
          }
        } else if(grabbed==Grip::VrMode) {
        toggleVrMode("chrome button");
        } else if(grabbed==Grip::ItemHandle) {
        grabItemOffset = itemOffset;
        } else if(grabbed!=Grip::Handle) {
        grabExtent = Tempest::Vec2(windowWidth,physicalHeight);
        }
      }
    }

  for(size_t i=0; i<hands.size(); ++i) {
    const Grip g = hands[i].hover;
    if(g==Grip::Handle || g==Grip::Environment || g==Grip::Dim || g==Grip::Aspect || g==Grip::Curve ||
       g==Grip::Hands || g==Grip::Edges || g==Grip::Inventory3D || g==Grip::SurfaceLight || g==Grip::VrMode)
      lastChromeHover = frame.predictedDisplayTime;
    }
  if(grabbed!=Grip::None && grabbed!=Grip::CornerLeft && grabbed!=Grip::CornerRight && grabbed!=Grip::ItemHandle)
    lastChromeHover = frame.predictedDisplayTime;

  // Ghost hands and rays: always, or (auto) after three quiet seconds of keyboard, mouse and
  // pad with a hand raised above table height
  handCapsules.clear();
  {
    const bool quiet = steadyNow-lastGameInput.load() > 3000000000;
    const float step = std::clamp(float(frame.predictedDisplayPeriod)*1e-9f*5.f, 0.02f, 0.3f);
    const float halfW = windowWidth*0.5f, halfH = windowHeight()*0.5f;
    auto capsule = [&](const XrVector3f& a, const XrVector3f& b, float radius, float opacity, V3 tint, bool ray = false) {
      handCapsules.push_back({{a.x,a.y,a.z,radius},{b.x,b.y,b.z,opacity},{tint.x,tint.y,tint.z,ray ? 1.f : 0.f}});
      };
    for(size_t i=0; i<hands.size() && i<2; ++i) {
      auto& h = hands[i];
      bool tracked = false;
      if(handTracker[i]!=XR_NULL_HANDLE && handsMode!=0) {
        XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = localSpace;
        li.time      = frame.predictedDisplayTime;
        XrHandJointLocationsEXT locs = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locs.jointCount     = XR_HAND_JOINT_COUNT_EXT;
        locs.jointLocations = joints[i].data();
        tracked = XR_SUCCEEDED(xrLocateHandJoints(handTracker[i],&li,&locs)) && locs.isActive &&
                  (joints[i][XR_HAND_JOINT_PALM_EXT].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        }
      const float y = tracked ? joints[i][XR_HAND_JOINT_PALM_EXT].pose.position.y
                              : (h.aimValid ? h.aim.position.y : -1e9f);
      const bool raised = headValid && y>headPos.y-0.45f;
      // A controller in pointer mode always shows its ray, even with hands and rays off
      const bool show = handsMode==2 || (handsMode==1 && quiet && raised) || (pointerMode && controllerHand[i]);
      handsAlpha[i] = std::clamp(handsAlpha[i]+(show ? step : -step), 0.f, 1.f);
      if(handsAlpha[i]<=0.f || (!tracked && !h.aimValid))
        continue;
      const float fade = handsAlpha[i];
      const V3 ghost{0.78f,0.88f,1.f};
      if(tracked) {
        auto bone = [&](int ja, int jb) {
          const auto& a = joints[i][size_t(ja)];
          const auto& b = joints[i][size_t(jb)];
          if(!(a.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(b.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            return;
          const float r = std::clamp(std::min(a.radius,b.radius), 0.004f, 0.02f);
          capsule(a.pose.position, b.pose.position, r, fade, ghost);
          };
        bone(XR_HAND_JOINT_WRIST_EXT, XR_HAND_JOINT_THUMB_METACARPAL_EXT);
        for(int j=XR_HAND_JOINT_THUMB_METACARPAL_EXT; j<XR_HAND_JOINT_THUMB_TIP_EXT; ++j)
          bone(j, j+1);
        for(int f : {int(XR_HAND_JOINT_INDEX_METACARPAL_EXT), int(XR_HAND_JOINT_MIDDLE_METACARPAL_EXT),
                     int(XR_HAND_JOINT_RING_METACARPAL_EXT), int(XR_HAND_JOINT_LITTLE_METACARPAL_EXT)}) {
          bone(XR_HAND_JOINT_WRIST_EXT, f);
          for(int j=f; j<f+4; ++j)
            bone(j, j+1);
          }
        // Across the knuckles, so the palm reads as a surface
        bone(XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT);
        bone(XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT, XR_HAND_JOINT_RING_PROXIMAL_EXT);
        bone(XR_HAND_JOINT_RING_PROXIMAL_EXT, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT);
        }
      // The ray, only towards the window and its controls (or the 3D item handle)
      if(h.aimValid && !vrActive) {
        if(h.hover==Grip::ItemHandle) {
          const V3 tint{1.f,0.88f,0.55f};
          capsule(h.aim.position, toXr(h.hit), 0.0012f, 0.7f*fade, tint, true);
          capsule(toXr(h.hit), toXr(h.hit), 0.008f, 0.9f*fade, tint, true);
          } else if(auto hit = rayHit(h.aim)) {
          const V3 l = unroll(toLocal(windowPose,*hit));
          // In pointer mode the ray reaches further around the window, so it can be walked onto the controls
          const float reach = pointerMode ? 0.4f : 0.f;
          if(std::abs(l.x)<halfW+0.4f+reach && l.y>-halfH-0.5f-reach && l.y<halfH+0.2f+reach) {
            const bool target = h.hover!=Grip::None;
            const V3 tint = target ? V3{1.f,0.88f,0.55f} : ghost;
            capsule(h.aim.position, toXr(*hit), 0.0012f, (target ? 0.7f : 0.45f)*fade, tint, true);
            capsule(toXr(*hit), toXr(*hit), target ? 0.008f : 0.006f, 0.9f*fade, tint, true);
            }
          }
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

  // In pointer mode a trigger aimed at (or dragging) a window control never reaches the game; aimed
  // elsewhere it still confirms in menus, the only way to on a controller without an A button.
  // Withheld like a lost action, a trigger still held when that ends must be released before it acts.
  auto trigger = [&](size_t id,int hand,const std::optional<float>& value,float& dst,float& old) {
    const bool onControl = pointerMode && (hands[size_t(hand)].hover!=Grip::None || grabbedBy==hand);
    if(admit(id,value.has_value() && !onControl,value.value_or(0.f)>0.35f,old>0.35f))
      dst = *value;
    else
      old = 0.f;
    };
  trigger(TriggerLeft, 0,hands[0].trigger,padState.lt,releasePad.lt);
  trigger(TriggerRight,1,hands[1].trigger,padState.rt,releasePad.rt);
  }

V3 XrWindow::Impl::handleCenter() const {
  return V3{0, -windowHeight()*0.5f + edgeFeather()*0.5f - 0.05f*chromeScale(), 0};
  }

Grip XrWindow::Impl::targetAt(float x, float y) const {
  const float halfW = windowWidth*0.5f;
  const float halfH = windowHeight()*0.5f;
  // Corners: a hand-sized zone around each bottom corner, mostly outside the image
  const float k = chromeScale();
  const float zone = 0.10f*k;
  const float inset = cornerInset();
  if(cornersReady && std::abs(y+halfH-inset)<zone) {
    if(std::abs(x-halfW+inset)<zone)
      return Grip::CornerRight;
    if(std::abs(x+halfW-inset)<zone)
      return Grip::CornerLeft;
    }
  const V3 hc = handleCenter();
  if(handleReady && std::abs(x-hc.x)<0.16f*k && std::abs(y-hc.y)<0.045f*k)
    return Grip::Handle;
  if(!chromeShown())
    return Grip::None;
  const V3 palm = handsCenter();
  if(handsButtonReady && std::abs(x-palm.x)<0.05f*k && std::abs(y-palm.y)<0.04f*k)
    return Grip::Hands;
  const V3 bend = curveCenter();
  if(curveReady && std::abs(x-bend.x)<0.05f*k && std::abs(y-bend.y)<0.04f*k)
    return Grip::Curve;
  const V3 edges = edgesCenter();
  if(chromeReady && std::abs(x-edges.x)<0.05f*k && std::abs(y-edges.y)<0.04f*k)
    return Grip::Edges;
  const V3 lock = aspectCenter();
  if(aspectReady && std::abs(x-lock.x)<0.05f*k && std::abs(y-lock.y)<0.04f*k)
    return Grip::Aspect;
  const V3 inv3d = inventory3DCenter();
  if(chromeReady && std::abs(x-inv3d.x)<0.05f*k && std::abs(y-inv3d.y)<0.04f*k)
    return Grip::Inventory3D;
  const V3 surf = surfaceLightCenter();
  if(chromeReady && hasDepth && std::abs(x-surf.x)<0.05f*k && std::abs(y-surf.y)<0.04f*k)
    return Grip::SurfaceLight;
  const V3 button = environmentCenter();
  if(buttonReady && roomAvailable() && std::abs(x-button.x)<0.055f*k && std::abs(y-button.y)<0.04f*k)
    return Grip::Environment;
  const V3 vr = vrModeCenter();
  if(chromeReady && std::abs(x-vr.x)<0.05f*k && std::abs(y-vr.y)<0.04f*k)
    return Grip::VrMode;
  const V3 slide = sliderCenter();
  if(sliderRow()>=0 && x>=slide.x+dimLeft()-0.025f*k && x<=slide.x+dimLeft()+dimTravel()+0.025f*k && std::abs(y-slide.y)<0.025f*k)
    return Grip::Dim;
  return Grip::None;
  }

void XrWindow::Impl::updateArmTargets() {
  // The arms follow the hands only while the wearer looks out of the hero's eyes (VR view)
  const bool enabled = vrActive && vrRefValid && hasHandTracking && xrLocateHandJoints!=nullptr &&
                       state==XR_SESSION_STATE_FOCUSED && AndroidApi::activityFocused() &&
                       Gothic::settingsGetI("INTERNAL","xrArmTracking")!=0;
  armTracking  = enabled;
  armHeadValid = false;
  for(auto& t:armTarget)
    t.valid = false;
  if(!enabled)
    return;

  XrSpaceLocation head = {XR_TYPE_SPACE_LOCATION};
  if(XR_SUCCEEDED(xrLocateSpace(viewSpace, localSpace, frame.predictedDisplayTime, &head)) &&
     (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
    armHead      = head.pose.orientation;
    armHeadValid = true;
    }

  constexpr XrSpaceLocationFlags posed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  for(size_t i=0; i<2; ++i) {
    if(handTracker[i]==XR_NULL_HANDLE)
      continue;
    auto& j = armJoints[i];
    XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    li.baseSpace = viewSpace;
    li.time      = frame.predictedDisplayTime;
    XrHandJointLocationsEXT locs = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    locs.jointCount     = XR_HAND_JOINT_COUNT_EXT;
    locs.jointLocations = j.data();
    // The tracker is inactive while controllers are held
    if(!XR_SUCCEEDED(xrLocateHandJoints(handTracker[i],&li,&locs)) || !locs.isActive)
      continue;
    const auto& wrist = j[XR_HAND_JOINT_WRIST_EXT];
    const auto& palm  = j[XR_HAND_JOINT_PALM_EXT];
    if((wrist.locationFlags & posed)!=posed || !(palm.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
      continue;

    // Curl: bend summed over the three finger joints (metacarpal..tip), ~230 degrees for a fist
    float bend = 0;
    int   fingers = 0;
    for(int f : {int(XR_HAND_JOINT_INDEX_METACARPAL_EXT), int(XR_HAND_JOINT_MIDDLE_METACARPAL_EXT),
                 int(XR_HAND_JOINT_RING_METACARPAL_EXT), int(XR_HAND_JOINT_LITTLE_METACARPAL_EXT)}) {
      bool valid = true;
      for(int k=0; k<5; ++k)
        valid &= (j[size_t(f+k)].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)!=0;
      if(!valid)
        continue;
      for(int k=0; k<3; ++k) {
        const V3 a = toV3(j[size_t(f+k)].pose.position), b = toV3(j[size_t(f+k+1)].pose.position), c = toV3(j[size_t(f+k+2)].pose.position);
        bend += std::acos(std::clamp(dot(normalize(b-a),normalize(c-b)), -1.f, 1.f));
        }
      ++fingers;
      }

    // Finger and thumb roots, or where they sit on an average hand
    auto joint = [&](XrHandJointEXT id, const V3& fallback) {
      const auto& l = j[size_t(id)];
      const V3 v = (l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ? toV3(l.pose.position) : toWorld(wrist.pose,fallback);
      return Vec3(v.x, v.y, v.z);
      };
    auto& t = armTarget[i];
    const auto& p = wrist.pose;
    t.valid       = true;
    t.wrist       = Vec3(p.position.x, p.position.y, p.position.z);
    t.orientation = Vec4(p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    t.palm        = Vec3(palm.pose.position.x, palm.pose.position.y, palm.pose.position.z);
    t.knuckle     = joint(XR_HAND_JOINT_INDEX_PROXIMAL_EXT, V3{i==0 ? 0.02f : -0.02f, 0.f, -0.09f});
    t.thumbBase   = joint(XR_HAND_JOINT_THUMB_METACARPAL_EXT, V3{i==0 ? 0.025f : -0.025f, -0.01f, -0.02f});
    t.curl        = fingers>0 ? std::clamp(bend/(float(fingers)*4.f), 0.f, 1.f) : 0.f;
    }
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
  const XrResult result = xrLocateViews(session, &info, &st, 2, &count, views);
  eyesValid = XR_SUCCEEDED(result) && count==2 &&
              (st.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
  if(traceWindowTracking) {
    const auto flags = XR_SUCCEEDED(result) ? st.viewStateFlags : XrViewStateFlags(0);
    Log::i("XR window tracking targetNs=",info.displayTime," result=",int(result)," flags=",uint32_t(flags),
           " views=",XR_SUCCEEDED(result) ? count : 0u," placed=",int(placed));
    if(eyesValid && (flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && placed) {
      for(uint32_t i=0; i<count; ++i) {
        const auto& p = views[i].pose.position;
        const auto& q = views[i].pose.orientation;
        const auto m = windowFovMargins(views[i],windowPose,windowWidth,windowHeight());
        Log::i("XR window eye targetNs=",info.displayTime," eye=",i," localM=",p.x,",",p.y,",",p.z,
               " quatXYZW=",q.x,",",q.y,",",q.z,",",q.w," marginsLRDUM=",m[0],",",m[1],",",m[2],",",m[3]);
        }
      }
    }
  if(!eyesValid)
    return;
  eyeViews[0] = views[0];
  eyeViews[1] = views[1];
  const XrQuaternionf inv = conjugate(windowPose.orientation);
  for(int i=0; i<2; ++i)
    eyeInWindow[i] = rotate(inv, toV3(views[i].pose.position) - toV3(windowPose.position));
  // Inject a known render-to-present position difference on a stationary headset.
  // The runtime still tracks the real eyes, and the window stays anchored in LOCAL.
  static const float testOffsetX = float(std::clamp(Gothic::settingsGetI("BENCHMARK","xrEyeOffsetMm"),-30,30))*0.001f;
  if(testOffsetX!=0.f) {
    for(auto& eye:eyeInWindow)
      eye.x += testOffsetX;
    }

  // The same view as a projection layer: from the eye, facing the window plane, with an
  // asymmetric field of view that frames exactly the window rectangle (used with SpaceWarp)
  const float halfW = windowWidth*0.5f, halfH = windowHeight()*0.5f;
  if(curved && curveRadius<=0.f && headValid)
    updateCurveRadius();
  for(int i=0; i<2; ++i) {
    V3 e = eyeInWindow[i];
    e.z  = std::max(e.z, 0.05f);
    auto& r = eyeRect[size_t(i)];
    r = {-halfW, halfW, -halfH, halfH};
    if(isCurved()) {
      // An ordinary perspective view from the eye, framed by the rectangle on the window plane
      // that just contains the curved window (its arcs and side edges projected from the eye)
      const float R = effectiveCurveRadius(), half = halfW/R;
      std::array<float,4> f = {1e9f,-1e9f,1e9f,-1e9f};
      auto add = [&](float a, float y) {
        const V3 x{R*std::sin(a), y, R*(1.f-std::cos(a))};
        const float dz = e.z-x.z;
        if(dz<0.05f)
          return;
        const float t = e.z/dz;
        const float px = e.x+(x.x-e.x)*t, py = e.y+(x.y-e.y)*t;
        f = {std::min(f[0],px), std::max(f[1],px), std::min(f[2],py), std::max(f[3],py)};
        };
      for(int k=0; k<=24; ++k) {
        const float a = -half+2.f*half*float(k)/24.f;
        add(a,-halfH);
        add(a, halfH);
        }
      for(int k=1; k<12; ++k) {
        const float y = -halfH+2.f*halfH*float(k)/12.f;
        add(-half,y);
        add( half,y);
        }
      if(f[0]<f[1] && f[2]<f[3]) {
        // A small margin keeps the outline off the image border
        const float mx = 0.01f*(f[1]-f[0]), my = 0.01f*(f[3]-f[2]);
        r = {f[0]-mx, f[1]+mx, f[2]-my, f[3]+my};
        }
      }
    eyePose[i].orientation = windowPose.orientation;
    eyePose[i].position    = toXr(toWorld(windowPose, e));
    eyeFov[i].angleLeft    = std::atan((r[0] - e.x)/e.z);
    eyeFov[i].angleRight   = std::atan((r[1] - e.x)/e.z);
    eyeFov[i].angleDown    = std::atan((r[2] - e.y)/e.z);
    eyeFov[i].angleUp      = std::atan((r[3] - e.y)/e.z);
    // Depth 0 is the near plane: the window plane, or the curve's nearest (side) edges
    eyeNear[i]             = std::max(e.z-curveSag(), 0.05f);
    }
  if(vrActive) {
    // VR: the game image is each eye's own full view (projection layer, life-size depth)
    for(int i=0; i<2; ++i) {
      eyePose[i] = views[i].pose;
      eyeFov[i]  = views[i].fov;
      eyeNear[i] = VrNear;
      }
    }
  }

bool XrWindow::eyeView(uint8_t eye, float verticalFovDeg, Camera::EyeView& out) const {
  auto& s = *impl;
  if(!s.eyesValid || eye>1)
    return false;
  // Scale picture magnifies the image in the window plane. Transform lateral eye motion by
  // the inverse magnification as well, so stereo still converges at the physical window.
  // Eye distance and depth stay in metres in both modes, including for SpaceWarp.
  const float units = 100.f;
  if(s.vrActive) {
    // VR: each eye from its tracked pose with the runtime's field of view, life-size, relative to the
    // head pose handed to the camera (updateVrView: camera angles hold its yaw and pitch)
    const auto&         v   = s.eyeViews[eye];
    const XrQuaternionf inv = conjugate(s.vrBase);
    const V3            p   = rotate(inv, toV3(v.pose.position) - s.vrBasePos)*units;
    const XrQuaternionf r   = mul(inv, v.pose.orientation);
    // View space of the game camera: +x right, +y down, +z forward; OpenXR: +y up, -z forward.
    // The rest of the eye's orientation (head roll, canted displays) is C*R^T*C, C = diag(1,-1,-1).
    out.offset = Tempest::Vec3(p.x, -p.y, -p.z);
    const V3 axis[3] = {rotate(r,V3{1,0,0}), rotate(r,V3{0,1,0}), rotate(r,V3{0,0,1})};
    auto m = [&](int i, int j) {
      const float flip = (i==0)==(j==0) ? 1.f : -1.f;
      const V3&   a    = axis[i];
      return flip*(j==0 ? a.x : (j==1 ? a.y : a.z));
      };
    out.rotation = Tempest::Matrix4x4(m(0,0), m(0,1), m(0,2), 0.f,
                                      m(1,0), m(1,1), m(1,2), 0.f,
                                      m(2,0), m(2,1), m(2,2), 0.f,
                                      0.f,    0.f,    0.f,    1.f);
    const float near = Impl::VrNear*units;
    out.zNear  = near;
    out.left   =  std::tan(v.fov.angleLeft)*near;
    out.right  =  std::tan(v.fov.angleRight)*near;
    out.bottom = -std::tan(v.fov.angleUp)*near;
    out.top    = -std::tan(v.fov.angleDown)*near;
    return true;
    }
  // Scale picture: one scale for both axes, chosen so the 2.4 x 1.35 m reference view always
  // fits; a wider window shows more to the sides, a taller one more above and below.
  const float fit    = std::max(2.4f/s.windowWidth, 1.35f/s.windowHeight());
  const float unitsX = s.scalePicture ? units*fit : units;
  const float unitsY = unitsX;
  const float halfW = s.windowWidth*0.5f*unitsX;
  const float halfH = s.windowHeight()*0.5f*unitsY;
  const float plane = Impl::portalDistance(verticalFovDeg)*units;

  V3 e = s.eyeInWindow[eye];
  e.z  = std::max(e.z, 0.05f);   // behind or inside the window: hold the eye just in front of it
  // View space of the game camera: +x right, +y down (Vulkan), +z forward
  const float ex = e.x*unitsX;
  const float ey = -e.y*unitsY;
  const float dz = e.z*units;    // eye distance to the window plane
  // Frame on the window plane (the window itself, or what contains its curve), at the near
  // plane: the window plane, or nearer by the curve's depth
  const auto& r    = s.eyeRect[eye];
  const float near = std::max(e.z-s.curveSag(), 0.05f)*units;
  const float k    = near/dz;
  (void)halfW;
  (void)halfH;
  out.offset = Tempest::Vec3(ex, ey, plane-dz);
  out.zNear  = near;
  out.left   = (r[0]*unitsX - ex)*k;
  out.right  = (r[1]*unitsX - ex)*k;
  out.bottom = (-r[3]*unitsY - ey)*k;
  out.top    = (-r[2]*unitsY - ey)*k;
  return true;
  }

void XrWindow::Impl::loadItemOffset() {
  if(itemOffsetLoaded)
    return;
  itemOffsetLoaded = true;
  if(Gothic::settingsGetI("INTERNAL","xrInventory3DPlaced")<2)
    return;
  const float x = Gothic::settingsGetF("INTERNAL","xrInventory3DOffsetX");
  const float y = Gothic::settingsGetF("INTERNAL","xrInventory3DOffsetY");
  const float z = Gothic::settingsGetF("INTERNAL","xrInventory3DOffsetZ");
  if(std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
    itemOffset = V3{std::clamp(x,-1.5f,1.5f), std::clamp(y,-1.0f,1.2f), std::clamp(z,0.12f,1.5f)};
  }

void XrWindow::Impl::saveItemOffset() {
  Gothic::settingsSetI("INTERNAL","xrInventory3DPlaced", 2);
  Gothic::settingsSetF("INTERNAL","xrInventory3DOffsetX", itemOffset.x);
  Gothic::settingsSetF("INTERNAL","xrInventory3DOffsetY", itemOffset.y);
  Gothic::settingsSetF("INTERNAL","xrInventory3DOffsetZ", itemOffset.z);
  Gothic::flushSettings();
  }

void XrWindow::drawInventoryItem(Encoder<CommandBuffer>& cmd, uint8_t eye, const Item* item) {
  auto& s = *impl;
  if(eye>1 || s.itemTargets[eye].empty() || !s.eyesValid || !s.placed) {
    if(eye==0)
      s.itemPreviewActive = false;
    return;
    }
  const bool enabled = Gothic::settingsGetI("INTERNAL","xrInventory3D")!=0;
  if(!enabled || item==nullptr) {
    if(eye==0) {
      s.itemPreviewActive = false;
      if(s.grabbed==Grip::ItemHandle) {
        s.saveItemOffset();
        s.grabbedBy = -1;
        s.grabbed   = Grip::None;
        }
      }
    return;
    }
  if(eye==0)
    s.loadItemOffset();

  const auto& itData = item->handle();
  if(s.cachedItemVisual!=itData.visual || s.cachedItemMesh==nullptr) {
    s.cachedItemVisual = itData.visual;
    s.cachedItemMesh   = Resources::loadMesh(itData.visual);
    if(s.cachedItemMesh!=nullptr) {
      const auto* bbox = s.cachedItemMesh->bbox();
      const float diag = (bbox[1]-bbox[0]).length();
      s.cachedItemMid  = (bbox[0]+bbox[1])*0.5f;
      s.cachedItemScale = diag>1e-4f ? (0.24f/diag) : 0.002f;
      }
    }
  if(s.cachedItemMesh==nullptr) {
    if(eye==0)
      s.itemPreviewActive = false;
    return;
    }
  if(eye==0)
    s.itemPreviewActive = true;

  if(s.itemAcquired[eye]<0) {
    std::lock_guard<std::mutex> guard(*s.vk.queueLock);
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(!ok(xrAcquireSwapchainImage(s.itemSwapchain[eye], &acq, &index), "acquire item preview image"))
      return;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    s.itemAcquired[eye] = int32_t(index);
    if(xrWaitSwapchainImage(s.itemSwapchain[eye], &wait)!=XR_SUCCESS)
      return;
    }
  if(s.itemPipeline.isEmpty()) {
    auto shader = [&](const char* name) {
      const auto code = GothicShader::get(name);
      return s.device->shader(code.data,code.len);
      };
    RenderState st;
    st.setCullFaceMode(RenderState::CullMode::NoCull);
    st.setZTestMode(RenderState::ZTestMode::LEqual);
    st.setZWriteEnabled(true);
    s.itemPipeline = s.device->pipeline(Triangles,st,shader("item_xr.vert.sprv"),shader("item_xr.frag.sprv"));
    }
  if(s.itemDepth.isEmpty())
    s.itemDepth = s.device->zbuffer(Tempest::TextureFormat::Depth16, Impl::ItemPreviewSize, Impl::ItemPreviewSize);

  // Stereo perspective projection (Vulkan clip, y down, z in [0, 1]) and eye view in LOCAL space
  const auto& v = s.eyeViews[eye];
  const float tL = std::tan(v.fov.angleLeft), tR = std::tan(v.fov.angleRight);
  const float tU = std::tan(v.fov.angleUp),   tD = std::tan(v.fov.angleDown);
  constexpr float zn = 0.05f, zf = 20.f;
  const float P[4][4] = {{2.f/(tR-tL), 0.f, (tR+tL)/(tR-tL), 0.f},
                         {0.f, -2.f/(tU-tD), -(tU+tD)/(tU-tD), 0.f},
                         {0.f, 0.f, -zf/(zf-zn), -(zf*zn)/(zf-zn)},
                         {0.f, 0.f, -1.f, 0.f}};
  const XrQuaternionf q = conjugate(v.pose.orientation);
  const V3 ex = rotate(q,V3{1,0,0}), ey = rotate(q,V3{0,1,0}), ez = rotate(q,V3{0,0,1});
  const V3 t  = rotate(q,toV3(v.pose.position));
  const float V[4][4] = {{ex.x, ey.x, ez.x, -t.x},
                         {ex.y, ey.y, ez.y, -t.y},
                         {ex.z, ey.z, ez.z, -t.z},
                         {0.f, 0.f, 0.f, 1.f}};
  float vpData[16] = {};
  for(int r=0; r<4; ++r)
    for(int c=0; c<4; ++c) {
      float sum = 0;
      for(int k=0; k<4; ++k)
        sum += P[r][k]*V[k][c];
      vpData[c*4+r] = sum;
      }
  const Tempest::Matrix4x4 vp(vpData);

  // Place item in front and slightly below the window (or wherever its handle was dragged)
  const XrQuaternionf wq = s.windowPose.orientation;
  const V3 wx = rotate(wq,V3{1,0,0}), wy = rotate(wq,V3{0,1,0}), wz = rotate(wq,V3{0,0,1});
  const V3 wpos = toWorld(s.windowPose, s.itemCenter());
  const float wm[16] = {wx.x,wx.y,wx.z,0.f, wy.x,wy.y,wy.z,0.f, wz.x,wz.y,wz.z,0.f, wpos.x,wpos.y,wpos.z,1.f};
  Tempest::Matrix4x4 baseModel(wm);
  const float spinDeg = float(std::fmod(double(s.frame.predictedDisplayTime)*1e-9*28.0, 360.0));
  baseModel.rotateOY(spinDeg);
  baseModel.scale(1.f, -1.f, 1.f); // InventoryRenderer::itemRotation targets Vulkan Y-down

  Tempest::Matrix4x4 itemMat = Tempest::Matrix4x4::mkIdentity();
  const auto rot = InventoryRenderer::itemRotation(itData);
  itemMat.rotateOX(rot.x);
  itemMat.rotateOZ(rot.z);
  itemMat.rotateOY(rot.y);
  for(int i=0; i<3; ++i) {
    const float trX = itemMat.at(i,0);
    const float trZ = itemMat.at(i,2);
    itemMat.set(i,0,trZ);
    itemMat.set(i,2,trX);
    }
  itemMat.scale(s.cachedItemScale);
  itemMat.translate(-s.cachedItemMid);
  baseModel.mul(itemMat);

  auto& target = s.itemTargets[eye][size_t(s.itemAcquired[eye])];
  cmd.setDebugMarker("XR 3D inventory item");
  cmd.setFramebuffer({{target, Vec4(0,0,0,0), Tempest::Preserve}}, {s.itemDepth, 1.f, Tempest::Discard});
  cmd.setViewport(0, 0, int(Impl::ItemPreviewSize), int(Impl::ItemPreviewSize));
  cmd.setPipeline(s.itemPipeline);

  struct Push { Tempest::Matrix4x4 mvp; Tempest::Matrix4x4 model; };
  auto drawAttach = [&](const ProtoMesh::Attach& att, const Tempest::Matrix4x4& model) {
    Push push;
    push.model = model;
    push.mvp   = vp;
    push.mvp.mul(model);
    cmd.setPushData(&push, sizeof(push));
    for(const auto& sub:att.sub) {
      if(sub.material.tex==nullptr || sub.iboLength==0)
        continue;
      cmd.setBinding(0, *sub.material.tex);
      cmd.draw(att.vbo, att.ibo, sub.iboOffset, sub.iboLength);
      }
    };

  const auto& pm = *s.cachedItemMesh;
  if(pm.nodes.empty()) {
    for(const auto& att:pm.attach)
      drawAttach(att, baseModel);
    } else {
    std::vector<Tempest::Matrix4x4> nodeTr(pm.nodes.size());
    for(size_t i=0; i<pm.nodes.size(); ++i) {
      const auto& n = pm.nodes[i];
      if(n.parentId<i) {
        nodeTr[i] = nodeTr[n.parentId];
        nodeTr[i].mul(n.transform);
        } else {
        nodeTr[i] = n.transform;
        }
      if(n.submeshIdB>=n.submeshIdE)
        continue;
      Tempest::Matrix4x4 m = baseModel;
      m.mul(nodeTr[i]);
      Push push;
      push.model = m;
      push.mvp   = vp;
      push.mvp.mul(m);
      cmd.setPushData(&push, sizeof(push));
      for(size_t sid=n.submeshIdB; sid<n.submeshIdE && sid<pm.submeshId.size(); ++sid) {
        const auto& ref = pm.submeshId[sid];
        if(ref.id>=pm.attach.size())
          continue;
        const auto& att = pm.attach[ref.id];
        if(ref.subId>=att.sub.size())
          continue;
        const auto& sub = att.sub[ref.subId];
        if(sub.material.tex==nullptr || sub.iboLength==0)
          continue;
        cmd.setBinding(0, *sub.material.tex);
        cmd.draw(att.vbo, att.ibo, sub.iboOffset, sub.iboLength);
        }
      }
    }
  s.itemDrawn[eye] = true;
  }

void XrWindow::drawHands(Encoder<CommandBuffer>& cmd, uint8_t eye, uint8_t slot) {
  auto& s = *impl;
  if(eye>1 || s.handCapsules.empty() || s.handsTargets[eye].empty() || !s.eyesValid)
    return;
  if(s.handsAcquired[eye]<0) {
    std::lock_guard<std::mutex> guard(*s.vk.queueLock);
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if(!ok(xrAcquireSwapchainImage(s.handsSwapchain[eye], &acq, &index), "acquire hands image"))
      return;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    s.handsAcquired[eye] = int32_t(index);
    if(xrWaitSwapchainImage(s.handsSwapchain[eye], &wait)!=XR_SUCCESS)
      return;
    }
  if(s.handsPipeline.isEmpty()) {
    auto shader = [&](const char* name) {
      const auto code = GothicShader::get(name);
      return s.device->shader(code.data,code.len);
      };
    RenderState st;
    st.setCullFaceMode(RenderState::CullMode::NoCull);
    st.setZTestMode(RenderState::ZTestMode::Always);
    st.setZWriteEnabled(false);
    st.setBlendSource(RenderState::BlendMode::One);
    st.setBlendDest(RenderState::BlendMode::OneMinusSrcAlpha);
    s.handsPipeline = s.device->pipeline(Triangles,st,shader("hand_ghost.vert.sprv"),shader("hand_ghost.frag.sprv"));
    st.setBlendDest(RenderState::BlendMode::Zero); // the fill replaces the outline under it
    s.handsFill = s.device->pipeline(Triangles,st,shader("hand_ghost.vert.sprv"),shader("hand_ghost.frag.sprv"));
    }
  auto& buf = s.handsBuffer[slot%s.handsBuffer.size()];
  const size_t bytes = s.handCapsules.size()*sizeof(Impl::Capsule);
  if(buf.byteSize()<bytes)
    buf = s.device->ssbo(Tempest::BufferHeap::Upload, s.handCapsules.data(), bytes);
  else
    buf.update(s.handCapsules.data(), 0, bytes);

  // This eye's view of the LOCAL space (Vulkan clip, y down; depth is not used)
  const auto& v = s.eyeViews[eye];
  const float tL = std::tan(v.fov.angleLeft), tR = std::tan(v.fov.angleRight);
  const float tU = std::tan(v.fov.angleUp),   tD = std::tan(v.fov.angleDown);
  const float P[4][4] = {{2.f/(tR-tL), 0.f, (tR+tL)/(tR-tL), 0.f},
                         {0.f, -2.f/(tU-tD), -(tU+tD)/(tU-tD), 0.f},
                         {0.f, 0.f, -0.5f, 0.f},
                         {0.f, 0.f, -1.f, 0.f}};
  const XrQuaternionf q = conjugate(v.pose.orientation);
  const V3 ex = rotate(q,V3{1,0,0}), ey = rotate(q,V3{0,1,0}), ez = rotate(q,V3{0,0,1});
  const V3 t  = rotate(q,toV3(v.pose.position));
  const float V[4][4] = {{ex.x, ey.x, ez.x, -t.x},
                         {ex.y, ey.y, ez.y, -t.y},
                         {ex.z, ey.z, ez.z, -t.z},
                         {0.f, 0.f, 0.f, 1.f}};
  struct Push { float m[16]; float size[4]; } push = {};
  for(int r=0; r<4; ++r)
    for(int c=0; c<4; ++c) {
      float sum = 0;
      for(int k=0; k<4; ++k)
        sum += P[r][k]*V[k][c];
      push.m[c*4+r] = sum;
      }
  const float n = float(Impl::HandsSize);
  push.size[0] = n;
  push.size[1] = n;
  push.size[2] = n/(tU-tD);

  auto& target = s.handsTargets[eye][size_t(s.handsAcquired[eye])];
  cmd.setDebugMarker("XR ghost hands");
  cmd.setFramebuffer({{target, Vec4(0,0,0,0), Tempest::Preserve}});
  cmd.setBinding(0, buf);
  // Dark hands as VRON Engine draws them: pale outlines, the dark fill over them, then rays
  for(int pass=0; pass<3; ++pass) {
    push.size[3] = float(pass);
    cmd.setPushData(&push, sizeof(push));
    cmd.setPipeline(pass==1 ? s.handsFill : s.handsPipeline);
    cmd.draw(nullptr, 0, 6, 0, s.handCapsules.size());
    }
  s.handsDrawn[eye] = true;
  }

Tempest::Rect XrWindow::uiViewport(uint8_t eye) const {
  auto& s = *impl;
  if(s.vrActive && s.eyesValid && eye<2 && s.width>0 && s.height>0) {
    // VR: the interface on a head-locked panel ahead, where each eye sees it (so it has depth)
    const auto& v = s.eyeViews[eye];
    const V3    e = rotate(conjugate(s.vrBase), toV3(v.pose.position)-s.vrBasePos); // eye in head space
    const float z = Impl::VrPanelDistance + e.z;
    const float halfW = Impl::VrPanelHalf*Impl::VrPanelDistance;
    const float halfH = halfW*float(s.height)/float(s.width);
    const float drop  = Impl::VrPanelDrop*Impl::VrPanelDistance;
    const float tl = std::tan(v.fov.angleLeft), tr = std::tan(v.fov.angleRight);
    const float td = std::tan(v.fov.angleDown), tu = std::tan(v.fov.angleUp);
    if(!(z>0.1f && tr>tl && tu>td))
      return Tempest::Rect();
    auto px = [&](float x) { return int(std::lround(((x-e.x)/z-tl)/(tr-tl)*float(s.width))); };
    auto py = [&](float y) { return int(std::lround((tu-(y-e.y)/z)/(tu-td)*float(s.height))); };
    const int x0 = px(-halfW), x1 = px(halfW), y0 = py(halfH-drop), y1 = py(-halfH-drop);
    return Tempest::Rect(x0, y0, std::max(1,x1-x0), std::max(1,y1-y0));
    }
  if(!s.isCurved() || eye>1 || s.width==0 || s.height==0)
    return Tempest::Rect();
  const auto& r = s.eyeRect[eye];
  const float halfW = s.windowWidth*0.5f, halfH = s.windowHeight()*0.5f;
  const float x0 = (-halfW-r[0])/(r[1]-r[0]), x1 = (halfW-r[0])/(r[1]-r[0]);
  const float y0 = (r[3]-halfH)/(r[3]-r[2]),  y1 = (r[3]+halfH)/(r[3]-r[2]);
  const int W = int(s.width), H = int(s.height);
  const int x = std::clamp(int(std::lround(x0*float(W))), 0, W-1);
  const int y = std::clamp(int(std::lround(y0*float(H))), 0, H-1);
  const int w = std::clamp(int(std::lround(x1*float(W)))-x, 1, W-x);
  const int h = std::clamp(int(std::lround(y1*float(H)))-y, 1, H-y);
  return Tempest::Rect(x, y, w, h);
  }

bool XrWindow::isWindowFilterSupported() const {
  return impl->hasWindowFilter;
  }

bool XrWindow::isFoveationSupported() const {
  return impl->rateTexel.width!=0;
  }

void XrWindow::prepareLightingRate(Encoder<CommandBuffer>& cmd, uint8_t slot, uint8_t eye, uint32_t w, uint32_t h) {
  auto& s = *impl;
  if(!isFoveationSupported() || !s.placed || !s.headValid || !s.eyesValid || eye>=2 || slot>=Resources::MaxFramesInFlight ||
     s.vrActive) // the rate map follows the window
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
  if(!placed || !headValid || moving || !focused || !frame.shouldRender || vrActive) {
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
  if(s.environmentDepth.takeFailure())
    Gothic::inst().onPrint("Surface light: no table or floor found below the window. Move the window above one.");
  if(!s.environmentDepth.surface())
    return;
  const auto& plane = *s.environmentDepth.surface();
  if(auto* target = s.surfaceLight.target(*s.device,s.session,s.eyeColorFormat,*s.vk.queueLock,s.frame.predictedDisplayTime)) {
    // The window's light reaches the surface from its lower part: about a quarter of its height
    // above its bottom edge. A curved window's light follows its arc.
    const Tempest::Vec4 footprint(plane.fullSize.width,plane.fullSize.height,s.windowWidth,
                                  s.isCurved() ? s.effectiveCurveRadius() : 0.f);
    const bool written = renderer.drawSurfaceLight(cmd,*target,{plane.uvOffset.x,plane.uvOffset.y},
                            {plane.uvScale.x,plane.uvScale.y},s.passthroughDim,plane.drop,s.eyeColorFormat==VK_FORMAT_R8G8B8A8_SRGB,
                            footprint,plane.drop+0.25f*s.windowHeight());
    s.surfaceLight.recorded(written);
    if(s.traceSurface) Log::i("XR surface texture recorded=",int(written));
    }
  }

int64_t XrWindow::nominalFramePeriod() const {
  const bool warpActive = impl->spaceWarpWanted ||
                          (impl->hasSpaceWarp && impl->sceneShown && Gothic::settingsGetI("INTERNAL","xrSpaceWarp")!=0);
  if(impl->refreshRate>=30.f && impl->refreshRate<=240.f)
    return int64_t(std::llround((warpActive ? 2e9 : 1e9)/double(impl->refreshRate)));
  // Core OpenXR reports the application frame period, including any runtime throttling.
  // For a missing prediction the frame clock can still estimate from consecutive targets.
  const auto period = impl->frame.predictedDisplayPeriod;
  return period>0 && period<=100000000 ? period : 0;
  }

int64_t XrWindow::predictedDisplayTime() const {
  return impl->frame.predictedDisplayTime;
  }

void XrWindow::Impl::applyRefreshRate() {
  // Video settings: 60, 72, 80, 90 or 120 Hz (the closest the headset supports). With
  // SpaceWarp the game renders at half of it.
  static constexpr float choices[] = {60.f, 72.f, 80.f, 90.f, 120.f};
  const int choice = std::clamp(Gothic::settingsGetI("INTERNAL","xrRefreshRate"),0,4);
  if(choice==requestedRateChoice || xrRequestRate==nullptr || supportedRates.empty())
    return;
  requestedRateChoice = choice;
  float best = supportedRates.front();
  for(float r:supportedRates)
    if(std::abs(r-choices[choice])<std::abs(best-choices[choice]))
      best = r;
  if(ok(xrRequestRate(session,best),"xrRequestDisplayRefreshRateFB"))
    Log::i("OpenXR: requested ", best, " Hz");
  }

bool XrWindow::waitFrame() {
  auto& s = *impl;
  if(!s.running || s.state==XR_SESSION_STATE_STOPPING) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return false;
    }
  if(s.state==XR_SESSION_STATE_FOCUSED || s.state==XR_SESSION_STATE_VISIBLE)
    s.applyRefreshRate();

  XrFrameWaitInfo wait = {XR_TYPE_FRAME_WAIT_INFO};
  s.frame = {XR_TYPE_FRAME_STATE};
  if(!s.sessionResult(xrWaitFrame(s.session, &wait, &s.frame), "xrWaitFrame"))
    return false;
  s.frameWaited = true;

  if(isVisible()) {
    const int style = Gothic::settingsGetI("INTERNAL","xrWindowEdges");
    const int nextStyle = (style==1 || style==2) ? style : 0;
    const bool contourChanged = s.edgeStyle!=nextStyle;
    s.edgeStyle = nextStyle;
    s.placeWindow();
    // No motion vector can describe an instantaneous change in source coverage.
    s.skipExtrapolation |= contourChanged;
    // A focused game keeps ticking even when the runtime requests zero layers.
    // Refresh XR input for this frame before simulation runs.
    s.updateInput();
    s.updateArmTargets();
    s.updateSurface();
    if(s.frame.shouldRender)
      return true;
    }
  // Not rendering: pair xrBeginFrame / xrEndFrame with zero layers immediately.
  {
  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
  if(!s.sessionResult(xrBeginFrame(s.session, &begin), "xrBeginFrame")) {
    s.frameWaited = false;
    return false;
    }
  }
  s.frameWaited = false;
  s.frameBegun  = true;
  submit(false);
  return false;
  }

bool XrWindow::beginFrame() {
  auto& s = *impl;
  if(!s.frameWaited)
    return false;
  s.frameWaited = false;
  {
  std::lock_guard<std::mutex> guard(*s.vk.queueLock);
  XrFrameBeginInfo begin = {XR_TYPE_FRAME_BEGIN_INFO};
  if(!s.sessionResult(xrBeginFrame(s.session, &begin), "xrBeginFrame"))
    return false;
  }
  s.frameBegun = true;

  if(isVisible() && s.frame.shouldRender) {
    s.locateEyes();
    return true;
    }
  submit(false);
  return false;
  }

void XrWindow::drawWindowEdges(Encoder<CommandBuffer>& cmd, bool motion) {
  auto& s = *impl;
  static const bool apertureGuard = Gothic::settingsGetI("BENCHMARK","xrApertureGuard")!=0;
  if((s.edgeStyle==0 && !(motion && apertureGuard) && !s.isCurved()) || s.vrActive)
    return;
  if(s.isCurved()) {
    if(s.curveCoverage.isEmpty()) {
      auto shader = [&](const char* name) {
        const auto code = GothicShader::get(name);
        return s.device->shader(code.data,code.len);
        };
      RenderState state;
      state.setCullFaceMode(RenderState::CullMode::NoCull);
      auto full = shader("triangle.vert.sprv");
      state.setBlendSource(RenderState::BlendMode::OneMinusDstColor);
      state.setBlendDest(RenderState::BlendMode::One);
      if(s.edgeOpaque.isEmpty())
        s.edgeOpaque = s.device->pipeline(Triangles,state,full,shader("window_opaque.frag.sprv"));
      state.setBlendSource(RenderState::BlendMode::Zero);
      state.setBlendDest(RenderState::BlendMode::SrcColor);
      s.curveCoverage = s.device->pipeline(Triangles,state,full,shader("window_curve.frag.sprv"));
      RenderState mstate;
      mstate.setCullFaceMode(RenderState::CullMode::NoCull);
      mstate.setZTestMode(RenderState::ZTestMode::Always);
      mstate.setZWriteEnabled(true);
      mstate.setBlendSource(RenderState::BlendMode::One);
      mstate.setBlendDest(RenderState::BlendMode::Zero);
      s.curveMotion = s.device->pipeline(Triangles,mstate,full,shader("window_curve_motion.frag.sprv"));
      }
    const uint8_t eye = s.recordingEye;
    V3 e = s.eyeInWindow[eye];
    e.z = std::max(e.z,0.05f);
    const auto& r = s.eyeRect[eye];
    const auto w = motion ? s.mvWidth : s.width;
    const auto h = motion ? s.mvHeight : s.height;
    struct Push {
      Vec4 rect, eye, shape;
      Vec2 size;
      uint32_t guard, pad;
      } push{{r[0],r[1],r[2],r[3]},{e.x,e.y,e.z,s.effectiveCurveRadius()},
             {s.windowWidth*0.5f,s.windowHeight()*0.5f,s.edgeRadius(),s.edgeFeather()},
             {float(w),float(h)},0u,0u};
    cmd.setDebugMarker(motion ? "XR curved window motion" : "XR curved window");
    if(!motion) {
      cmd.setPipeline(s.edgeOpaque);
      cmd.draw(nullptr,0,3);
      }
    cmd.setPipeline(motion ? s.curveMotion : s.curveCoverage);
    cmd.setPushData(push);
    cmd.draw(nullptr,0,3);
    return;
    }
  if((motion ? s.edgeMotion : s.edgeCoverage).isEmpty()) {
    auto shader = [&](const char* name) {
      const auto code = GothicShader::get(name);
      return s.device->shader(code.data,code.len);
      };
    RenderState state;
    state.setCullFaceMode(RenderState::CullMode::NoCull);
    auto border = shader("window_edge.vert.sprv");
    state.setZWriteEnabled(motion);
    if(motion) {
      state.setZTestMode(RenderState::ZTestMode::Always);
      state.setBlendSource(RenderState::BlendMode::One);
      state.setBlendDest(RenderState::BlendMode::Zero);
      s.edgeMotion = s.device->pipeline(Triangles,state,border,shader("window_edge_motion.frag.sprv"));
      } else {
      state.setBlendSource(RenderState::BlendMode::OneMinusDstColor);
      state.setBlendDest(RenderState::BlendMode::One);
      s.edgeOpaque = s.device->pipeline(Triangles,state,shader("triangle.vert.sprv"),shader("window_opaque.frag.sprv"));
      state.setBlendSource(RenderState::BlendMode::Zero);
      state.setBlendDest(RenderState::BlendMode::SrcColor);
      s.edgeCoverage = s.device->pipeline(Triangles,state,border,shader("window_edge.frag.sprv"));
      }
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
    uint32_t guard;
    } push{{s.windowWidth,s.windowHeight()},{s.windowWidth/float(w),s.windowHeight()/float(h)},
           s.edgeRadius(),s.edgeFeather(),uint32_t(motion && apertureGuard)};
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
  for(auto fill : {&Impl::fillHandle, &Impl::fillCorners, &Impl::fillEnvironment}) {
    try {
      (this->*fill)(cmd,fence);
      }
    catch(const std::bad_alloc&) {
      Log::e("OpenXR: insufficient memory for optional window graphics");
      }
    }
  }

void XrWindow::Impl::fillEnvironment(VkCommandBuffer cmd, VkFence fence) {
  chromeReady      = chromeSwapchain!=XR_NULL_HANDLE;
  handsMode        = std::clamp(Gothic::settingsGetI("INTERNAL","xrHands"),0,2);
  handsButtonReady = chromeReady;
  curveLevel       = std::clamp(Gothic::settingsGetI("INTERNAL","xrCurved"),0,2);
  curved           = curveLevel>0;
  if(curved && placed && headValid)
    updateCurveRadius();
  curveReady       = chromeReady;
  aspectLocked       = Gothic::settingsGetI("INTERNAL","xrAspectLock")!=0;
  aspectReady        = chromeReady;
  edgeStyle          = std::clamp(Gothic::settingsGetI("INTERNAL","xrWindowEdges"),0,2);
  inventory3DEnabled = Gothic::settingsGetI("INTERNAL","xrInventory3D")!=0;
  surfaceLightEnabled= Gothic::settingsGetI("INTERNAL","xrSurfaceLight")!=0;
  const float saved  = Gothic::settingsGetF("INTERNAL","xrPassthroughDim");
  passthroughDim   = std::isfinite(saved) ? std::clamp(saved,0.f,1.f) : 0.f;

  if(panorama!=XR_NULL_HANDLE)
    panoramaReady = uploadStatic(vk, cmd, fence, panorama, SkyW, SkyH, 1, duskPanorama(SkyW,SkyH));
  buttonReady = chromeReady && panoramaReady && (hasPassthrough || blendRoom);
  sliderReady = buttonReady && xrSetPassthroughStyle!=nullptr;
  if(sliderReady && passthroughDim>0.f)
    applyDim(passthroughDim);
  // Settings exist by the first rendered frame. An incomplete button should
  // never strand a wearer in VR when passthrough is available.
  if(buttonReady && Gothic::settingsGetI("INTERNAL","xrEnvironment")==1)
    restoreSky = !setEnvironment(true);
  }

void XrWindow::Impl::updateChromePanel(bool showEnv, bool showSlider, bool showSurfaceLight, Grip hover) {
  if(!chromeReady || chromeSwapchain==XR_NULL_HANDLE)
    return;
  ChromeState st;
  st.showEnv          = showEnv;
  st.immersive        = immersive;
  st.aspectLocked     = aspectLocked;
  st.showSlider       = showSlider;
  st.inventory3D      = inventory3DEnabled;
  st.showSurfaceLight = showSurfaceLight;
  st.surfaceLightOn   = surfaceLightEnabled;
  st.vrMode           = vrMode;
  st.handsMode        = handsMode;
  st.curveLevel       = curveLevel;
  st.edgeStyle        = edgeStyle;
  st.dimStep          = showSlider ? int(std::lround(passthroughDim*200.f)) : -1;
  st.hover            = hover;
  if(chromeUploaded && st==lastChromeState)
    return;

  std::vector<uint32_t> px(size_t(ChromeW)*ChromeH, 0u);
  constexpr float ppm = float(ChromeW)/ChromePhysW; // 1000 px/m (exact square pixels)
  constexpr float btnScale = (0.096f*ppm)/float(ButtonW);
  constexpr float btnCy = float(ChromeH)*0.5f - 0.0325f*ppm;

  auto seg = [](float u, float v, float ax, float ay, float bx, float by) {
    const float dx = bx-ax, dy = by-ay;
    const float t = std::clamp(((u-ax)*dx+(v-ay)*dy)/(dx*dx+dy*dy), 0.f, 1.f);
    return std::hypot(u-ax-dx*t, v-ay-dy*t);
    };

  auto drawButton = [&](float lx, bool isHover, auto&& iconSdf) {
    const float cx = float(ChromeW)*0.5f + lx*ppm;
    const int x0 = std::max(0, int(std::floor(cx - float(ButtonW)*0.5f*btnScale)) - 1);
    const int x1 = std::min(int(ChromeW), int(std::ceil(cx + float(ButtonW)*0.5f*btnScale)) + 1);
    const int y0 = std::max(0, int(std::floor(btnCy - float(ButtonH)*0.5f*btnScale)) - 1);
    const int y1 = std::min(int(ChromeH), int(std::ceil(btnCy + float(ButtonH)*0.5f*btnScale)) + 1);
    for(int y=y0; y<y1; ++y)
      for(int x=x0; x<x1; ++x) {
        const float u = (float(x)+0.5f - cx)/btnScale;
        const float v = (float(y)+0.5f - btnCy)/btnScale;
        const float rim = std::hypot(std::max(std::abs(u)-22.f,0.f), v);
        if(rim>35.5f)
          continue;
        const float ink = iconSdf(u, v);
        const float edge = std::clamp(1.5f-std::abs(rim-32.f),0.f,1.f);
        const float light = std::max(ink, edge*0.62f);
        px[size_t(y)*ChromeW+size_t(x)] =
            pixel(0.12f+0.84f*light, 0.15f+0.82f*light, 0.20f+0.8f*light,
                  isHover ? 0.97f : 0.12f+0.16f*light, 34.f-rim);
        }
    };

  // 1. Hands button (-0.385 m)
  if(handsButtonReady) {
    const int mode = handsMode;
    drawButton(-0.385f, hover==Grip::Hands, [&](float u, float v) {
      float d = std::max(std::abs(u)-6.f, std::abs(v-6.f)-5.f);
      d = std::min(d, seg(u,v,-6.f,0.f,-7.f,-11.f)-1.6f);
      d = std::min(d, seg(u,v,-2.f,0.f,-2.f,-14.f)-1.6f);
      d = std::min(d, seg(u,v, 2.f,0.f, 2.f,-13.f)-1.6f);
      d = std::min(d, seg(u,v, 6.f,0.f, 7.f,-10.f)-1.6f);
      d = std::min(d, seg(u,v,-6.f,8.f,-13.f,2.f)-1.8f);
      if(mode==0)
        d = std::min(d, seg(u,v,-14.f,13.f,14.f,-13.f)-1.4f);
      if(mode==1)
        d = std::min(d, std::hypot(u-15.f,v+10.f)-3.f);
      return std::clamp(0.5f-d, 0.f, 1.f);
      });
    }

  // 2. Curve button (-0.275 m)
  if(curveReady) {
    const int level = curveLevel;
    drawButton(-0.275f, hover==Grip::Curve, [&](float u, float v) {
      float line;
      if(level>0) {
        const float R = level>=2 ? 17.f : 26.f;
        line = std::abs(std::hypot(u, v-R+8.f)-R)-2.f;
        line = std::max(line, std::abs(u)-17.f);
        } else {
        line = std::max(std::abs(v+2.f)-2.f, std::abs(u)-17.f);
        }
      const float eye = std::hypot(u, v-11.f)-2.5f;
      return std::clamp(0.5f-std::min(line,eye), 0.f, 1.f);
      });
    }

  // 3. Corner / edge style button (-0.165 m): square, rounded, or rounded + soft edge
  {
    const int style = edgeStyle;
    drawButton(-0.165f, hover==Grip::Edges, [&](float u, float v) {
      // Rounded rectangle outline; corner radius grows from 0 (square) to 9 (rounded / soft)
      const float rad = style==0 ? 0.f : 9.f;
      const float qx = std::abs(u) - (16.f - rad);
      const float qy = std::abs(v) - (12.f - rad);
      const float box = std::hypot(std::max(qx,0.f), std::max(qy,0.f)) + std::min(std::max(qx,qy),0.f) - rad;
      const float outline = std::abs(box) - 1.9f;
      float ink = std::clamp(0.5f - outline, 0.f, 1.f);
      if(style==2) {
        // Soft outer feather glow around the rounded outline
        const float glow = std::clamp(1.f - std::abs(box)/7.5f, 0.f, 1.f);
        ink = std::max(ink, glow*0.65f);
        }
      return ink;
      });
    }

  // 4. Aspect lock button (-0.055 m)
  if(aspectReady) {
    const bool locked = aspectLocked;
    drawButton(-0.055f, hover==Grip::Aspect, [&](float u, float v) {
      const float body = std::max(std::abs(u)-11.f, std::abs(v-5.f)-8.f);
      const float sx = locked ? 0.f : 5.f, sy = locked ? 0.f : -5.f;
      const float au = u-sx, av = v-sy+3.f;
      const float arch = av<0.f ? std::abs(std::hypot(au,av)-8.f)-2.f
                                : std::abs(std::abs(au)-8.f)-2.f + (av>4.f ? 99.f : 0.f);
      return std::clamp(0.5f-std::min(body,arch), 0.f, 1.f);
      });
    }

  // 5. 3D Inventory preview button (+0.055 m): isometric 3D cube (crossed when off)
  {
    const bool on = inventory3DEnabled;
    drawButton(0.055f, hover==Grip::Inventory3D, [&](float u, float v) {
      // Isometric cube vertices
      const float r = 14.f, dx = r*0.866f, dy = r*0.5f;
      float d = seg(u,v, 0.f,-r,  dx,-dy);
      d = std::min(d, seg(u,v, dx,-dy, dx, dy));
      d = std::min(d, seg(u,v, dx, dy, 0.f, r));
      d = std::min(d, seg(u,v, 0.f, r,-dx, dy));
      d = std::min(d, seg(u,v,-dx, dy,-dx,-dy));
      d = std::min(d, seg(u,v,-dx,-dy, 0.f,-r));
      // Inner Y edges meeting at center
      d = std::min(d, seg(u,v, 0.f, 0.f, dx,-dy));
      d = std::min(d, seg(u,v, 0.f, 0.f,-dx,-dy));
      d = std::min(d, seg(u,v, 0.f, 0.f, 0.f, r));
      d -= 1.6f;
      if(!on)
        d = std::min(d, seg(u,v,-15.f,13.f,15.f,-13.f)-1.5f);
      return std::clamp(0.5f-d, 0.f, 1.f);
      });
    }

  // 6. Surface light button (+0.165 m): screen bar casting cone onto table (crossed when off)
  if(showSurfaceLight) {
    const bool on = surfaceLightEnabled;
    drawButton(0.165f, hover==Grip::SurfaceLight, [&](float u, float v) {
      float d = seg(u,v,-12.f,-11.f, 12.f,-11.f)-2.0f; // screen top bar
      d = std::min(d, seg(u,v,-17.f, 11.f, 17.f, 11.f)-1.6f); // table surface line
      d = std::min(d, seg(u,v,-10.f,-8.f, -15.f, 9.f)-1.3f);  // left cone ray
      d = std::min(d, seg(u,v, 10.f,-8.f,  15.f, 9.f)-1.3f);  // right cone ray
      d = std::min(d, seg(u,v,  0.f,-8.f,   0.f, 9.f)-1.3f);  // center ray
      if(!on)
        d = std::min(d, seg(u,v,-15.f,13.f,15.f,-13.f)-1.5f);
      return std::clamp(0.5f-d, 0.f, 1.f);
      });
    }

  // 7. Environment button (+0.275 m)
  if(showEnv) {
    const bool sky = immersive;
    drawButton(0.275f, hover==Grip::Environment, [&](float u, float v) {
      const float moon = std::min(16.f-std::hypot(u,v), std::hypot(u-8.f,v+5.f)-14.f);
      const float window = 2.f-std::abs(std::max(std::abs(u)-17.f, std::abs(v)-12.f));
      return std::clamp((sky ? window : moon)+0.5f, 0.f, 1.f);
      });
    }

  // 8. Enter VR button (+0.385 m): a headset, front view (the button hides with the window in VR).
  // A dot: VR mode is on and starts once the game shows its free camera again.
  const bool vrPending = vrMode;
  drawButton(0.385f, hover==Grip::VrMode, [&](float u, float v) {
    const float qx = std::abs(u)-14.f, qy = std::abs(v+1.f)-8.f;
    const float body = std::abs(std::hypot(std::max(qx,0.f),std::max(qy,0.f)) + std::min(std::max(qx,qy),0.f) - 5.f) - 1.8f;
    const float lens = std::hypot(std::abs(u)-7.5f, v+1.f) - 4.f;
    const float strap = std::max(std::abs(v+1.f)-2.f, 19.f-std::abs(u)) + (std::abs(u)>22.f ? 99.f : 0.f);
    const float dot   = vrPending ? std::hypot(u-25.f, v-12.f)-3.f : 99.f;
    return std::clamp(0.5f-std::min({body,lens,strap,dot}), 0.f, 1.f);
    });

  // Dimming slider below the button row
  if(showSlider) {
    const float scx = float(ChromeW)*0.5f;
    const float scy = float(ChromeH)*0.5f + 0.0375f*ppm;
    const float railHalf = 0.10f*ppm;
    const float railR    = 6.8f;
    const float knobX    = scx + (-0.10f + 0.20f*passthroughDim)*ppm;
    const float knobR    = 13.5f;
    const int y0 = std::max(0, int(std::floor(scy - knobR - 2.f)));
    const int y1 = std::min(int(ChromeH), int(std::ceil(scy + knobR + 2.f)));
    const int x0 = std::max(0, int(std::floor(scx - railHalf - knobR - 2.f)));
    const int x1 = std::min(int(ChromeW), int(std::ceil(scx + railHalf + knobR + 2.f)));
    for(int y=y0; y<y1; ++y)
      for(int x=x0; x<x1; ++x) {
        const float fx = float(x)+0.5f, fy = float(y)+0.5f;
        const float dy = fy - scy;
        const float rd = std::hypot(std::max(std::abs(fx-scx)-railHalf, 0.f), dy);
        const float kd = std::hypot(fx-knobX, dy);
        // Knob composites over the rail
        const float ka = std::clamp(knobR - kd + 0.5f, 0.f, 1.f);
        if(ka>0.f) {
          const float ink = std::clamp(knobR-3.f-kd, 0.f, 1.f);
          px[size_t(y)*ChromeW+size_t(x)] =
              pixel(0.12f+0.83f*ink, 0.15f+0.82f*ink, 0.20f+0.8f*ink, 0.96f, knobR-kd+0.5f);
          } else if(rd<railR+1.f) {
          const float ink = std::clamp(1.6f-std::abs(rd-railR+1.3f), 0.f, 1.f)*0.45f;
          px[size_t(y)*ChromeW+size_t(x)] =
              pixel(0.12f+0.83f*ink, 0.15f+0.82f*ink, 0.20f+0.8f*ink, 0.55f, railR-rd+0.5f);
          }
        }
    }

  VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = vk.queueFamily;
  VkCommandPool pool = VK_NULL_HANDLE;
  if(vkCreateCommandPool(vk.device, &pi, nullptr, &pool)!=VK_SUCCESS)
    return;
  struct Guard {
    VkDevice dev; VkCommandPool pool; VkFence fence = VK_NULL_HANDLE;
    ~Guard() { vkDestroyFence(dev, fence, nullptr); vkDestroyCommandPool(dev, pool, nullptr); }
    } g{vk.device, pool};
  VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if(vkAllocateCommandBuffers(vk.device, &alloc, &cb)!=VK_SUCCESS ||
     vkCreateFence(vk.device, &fi, nullptr, &g.fence)!=VK_SUCCESS)
    return;
  if(uploadStatic(vk, cb, g.fence, chromeSwapchain, ChromeW, ChromeH, 1, px)) {
    lastChromeState = st;
    chromeUploaded  = true;
    }
  }

void XrWindow::Impl::fillHandle(VkCommandBuffer cmd, VkFence fence) {
  // Handle: a capsule; idle, hover and held states
  if(handleSwapchain!=XR_NULL_HANDLE) {
    const uint32_t w = HandleW, h = HandleH;
    std::vector<uint32_t> px(size_t(w)*h*3);
    const float r   = float(h)*0.5f - 2.f;
    const float len = float(w)*0.5f - float(h)*0.5f;
    // Idle: very faint, so it does not draw the eye; hovered and held: clearly visible
    const struct { float r,g,b,o; } st[3] = {{0.9f,0.9f,0.9f,0.06f}, {1.f,1.f,1.f,0.9f}, {0.8f,0.9f,1.f,1.f}};
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
  s.recordingEye = eye;
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
  std::array<const XrCompositionLayerBaseHeader*,16> layers;
  uint32_t layerCount = 0;
  auto addLayer = [&](const auto& layer) {
    layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    };

  XrCompositionLayerPassthroughFB pt = {XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
  XrCompositionLayerEquirect2KHR sky = {XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
  XrCompositionLayerEquirectKHR sky1 = {XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR};
  XrCompositionLayerColorScaleBiasKHR skyDim = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
  bool alphaRoom = false;
  // VR mode: the game fills the view, opaque, without the window's frame parts
  const bool vrView = hasImage && s.vrActive;
  if(vrView) {
    // nothing of the room or the sky behind the game
    } else if(s.panoramaReady && (s.immersive || !s.roomAvailable())) {
    if(s.hasColorScale && s.passthroughDim>0.f) {
      const float k = 1.f-0.92f*s.passthroughDim;
      skyDim.colorScale = {k,k,k,1.f};
      sky.next  = &skyDim;
      sky1.next = &skyDim;
      }
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
    } else if(s.blendsRoom()) {
    // No bottom layer: the room shows wherever no layer covers it. The window quad is opaque,
    // every layer drawn around it blends by source alpha.
    alphaRoom = true;
    } else if(s.roomAvailable()) {
    pt.layerHandle = s.passthroughLayer;
    if(s.dimUsesOpacity && s.passthroughDim>0.f)
      pt.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    addLayer(pt);
    }

  const XrCompositionLayerQuad* spill = nullptr;
  if(hasImage && !vrView && s.lightEnabled && s.environmentDepth.surface()) {
    const auto& surface = *s.environmentDepth.surface();
    spill = s.surfaceLight.layer(s.localSpace,surface.pose,surface.size,s.hasAlphaBlend);
    if(spill)
      addLayer(*spill);
    }
  // The same rectangle in the room for both eyes, each showing its own view through it
  XrCompositionLayerQuad quad[2] = {{XR_TYPE_COMPOSITION_LAYER_QUAD},{XR_TYPE_COMPOSITION_LAYER_QUAD}};
  // SpaceWarp: the window as a projection layer with motion vectors and depth
  XrCompositionLayerProjection      proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  const bool curvedView = hasImage && s.isCurved();
  if((s.edgeStyle!=0 || curvedView) && !vrView) {
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
  if(warp || curvedView || vrView) {
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
      v.next                     = warp ? &w : nullptr;
      v.pose                     = s.eyePose[eye];
      v.fov                      = s.eyeFov[eye];
      v.subImage.swapchain       = s.swapchain[eye];
      v.subImage.imageRect       = {{0,0},{int32_t(s.width),int32_t(s.height)}};
      if(curvedView) {
        // The image is usually smaller than its swapchain: keep the compositor's filter from
        // reaching stale texels beyond it (a faint frame around the curved window)
        v.subImage.imageRect = {{1,1},{int32_t(s.width)-2,int32_t(s.height)-2}};
        }
      v.subImage.imageArrayIndex = 0;
      }
    proj.space     = s.localSpace;
    proj.viewCount = 2;
    proj.views     = pv;
    addLayer(proj);
    }

  if(hasImage && !warp && !curvedView && !vrView) {
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
  const float reveal = s.chromeReveal();
  XrCompositionLayerColorScaleBiasKHR fade = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
  fade.colorScale = {1.f,1.f,1.f,reveal};
  const void* fadeNext = s.hasColorScale && reveal<1.f ? &fade : nullptr;
  const bool showButtons = hasImage && reveal>0.f && !vrView;
  XrCompositionLayerQuad chromePanel = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  if(s.chromeReady && showButtons) {
    const bool showEnv          = s.buttonReady && s.roomAvailable();
    const bool showSlider       = s.sliderRow()>=0;
    const bool showSurfaceLight = s.hasDepth;
    Grip chromeHover = Grip::None;
    for(const Grip g : {s.grabbed, s.hands[0].hover, s.hands[1].hover}) {
      if(g==Grip::Hands || g==Grip::Curve || g==Grip::Edges || g==Grip::Aspect ||
         g==Grip::Inventory3D || g==Grip::SurfaceLight || g==Grip::Environment || g==Grip::Dim || g==Grip::VrMode) {
        chromeHover = g;
        break;
        }
      }
    s.updateChromePanel(showEnv, showSlider, showSurfaceLight, chromeHover);
    if(s.chromeUploaded) {
      chromePanel.next = fadeNext;
      chromePanel.layerFlags = chromeFlags;
      chromePanel.space = s.localSpace;
      chromePanel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      chromePanel.subImage.swapchain = s.chromeSwapchain;
      chromePanel.subImage.imageRect = {{0,0},{int32_t(Impl::ChromeW),int32_t(Impl::ChromeH)}};
      chromePanel.pose = s.windowPose;
      chromePanel.pose.position = toXr(toWorld(s.windowPose, s.chromeCenter()));
      chromePanel.size = {Impl::ChromePhysW*s.chromeScale(), Impl::ChromePhysH*s.chromeScale()};
      addLayer(chromePanel);
      }
    }
  XrCompositionLayerQuad handle = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  std::array<XrCompositionLayerQuad,2> corners;
  if(s.handleReady && hasImage && !vrView) {
    const bool hoverHandle = s.hands[0].hover==Grip::Handle || s.hands[1].hover==Grip::Handle;

    handle.layerFlags                = chromeFlags;
    handle.space                     = s.localSpace;
    handle.eyeVisibility             = XR_EYE_VISIBILITY_BOTH;
    handle.subImage.swapchain        = s.handleSwapchain;
    handle.subImage.imageRect        = {{0,0},{int32_t(Impl::HandleW),int32_t(Impl::HandleH)}};
    handle.subImage.imageArrayIndex  = s.grabbed==Grip::Handle ? 2u : (hoverHandle ? 1u : 0u);
    handle.pose.orientation          = s.windowPose.orientation;
    handle.pose.position             = toXr(toWorld(s.windowPose, s.handleCenter()));
    handle.size                      = {0.22f*s.chromeScale(), 0.0275f*s.chromeScale()};
    addLayer(handle);
    }

  if(s.cornersReady && hasImage && !vrView) {
    const float halfW = s.windowWidth*0.5f, halfH = s.windowHeight()*0.5f, sz = 0.10f*s.chromeScale();
    for(int c=0; c<2; ++c) {
      const auto corner = c==0 ? Grip::CornerLeft : Grip::CornerRight;
      const bool held = s.grabbed==corner;
      if(!held && s.hands[0].hover!=corner && s.hands[1].hover!=corner)
        continue;
      // The bracket's inner corner sits just outside the window corner
      const float sx = c==0 ? -1.f : 1.f;
      const float k  = sz*(44.f/64.f - 0.5f);   // bracket corner offset from the quad centre
      const float inset = s.cornerInset();
      const V3 at{sx*(halfW - inset + 0.012f*s.chromeScale() - k), -halfH + inset - 0.012f*s.chromeScale() + k, 0.002f};
      auto& q = corners[size_t(c)];
      q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      q.layerFlags               = chromeFlags;
      q.space                    = s.localSpace;
      q.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
      q.subImage.swapchain       = s.cornerSwapchain;
      q.subImage.imageRect       = {{0,0},{int32_t(Impl::CornerSize),int32_t(Impl::CornerSize)}};
      q.subImage.imageArrayIndex = uint32_t((c==0 ? 2 : 0) + (held ? 1 : 0));
      // On a curved window the bracket sits on the arc, facing its axis
      q.pose.orientation         = s.isCurved() ? mul(s.windowPose.orientation, aroundY(-at.x/s.effectiveCurveRadius()))
                                                : s.windowPose.orientation;
      q.pose.position            = toXr(toWorld(s.windowPose, s.roll(at)));
      q.size                     = {sz, sz};
      addLayer(q);
      }
    }

  XrCompositionLayerProjection     itemLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView itemView[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerQuad           itemHandle = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  const bool itemShown = hasImage && s.itemDrawn[0] && s.itemDrawn[1];
  for(int eye=0; eye<2; ++eye) {
    s.itemDrawn[eye] = false;
    if(!releaseImage(s.itemSwapchain[eye], s.itemAcquired[eye]))
      SystemApi::exit();
    }
  if(itemShown && layerCount<layers.size()) {
    for(int eye=0; eye<2; ++eye) {
      auto& v = itemView[eye];
      v.pose = s.eyeViews[eye].pose;
      v.fov  = s.eyeViews[eye].fov;
      v.subImage.swapchain = s.itemSwapchain[eye];
      v.subImage.imageRect = {{0,0},{int32_t(Impl::ItemPreviewSize),int32_t(Impl::ItemPreviewSize)}};
      }
    itemLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied
    itemLayer.space      = s.localSpace;
    itemLayer.viewCount  = 2;
    itemLayer.views      = itemView;
    addLayer(itemLayer);
    if(s.handleReady && layerCount<layers.size()) {
      const bool hoverItem = s.hands[0].hover==Grip::ItemHandle || s.hands[1].hover==Grip::ItemHandle;
      itemHandle.layerFlags               = chromeFlags;
      itemHandle.space                    = s.localSpace;
      itemHandle.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
      itemHandle.subImage.swapchain       = s.handleSwapchain;
      itemHandle.subImage.imageRect       = {{0,0},{int32_t(Impl::HandleW),int32_t(Impl::HandleH)}};
      itemHandle.subImage.imageArrayIndex = s.grabbed==Grip::ItemHandle ? 2u : (hoverItem ? 1u : 0u);
      itemHandle.pose.orientation         = s.windowPose.orientation;
      itemHandle.pose.position            = toXr(toWorld(s.windowPose, s.itemHandleCenter()));
      itemHandle.size                     = {0.11f, 0.015f};
      addLayer(itemHandle);
      }
    }

  XrCompositionLayerProjection     handsLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView handsView[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  const bool handsShown = hasImage && s.handsDrawn[0] && s.handsDrawn[1];
  for(int eye=0; eye<2; ++eye) {
    s.handsDrawn[eye] = false;
    if(!releaseImage(s.handsSwapchain[eye], s.handsAcquired[eye]))
      SystemApi::exit();
    }
  if(handsShown && layerCount<layers.size()) {
    for(int eye=0; eye<2; ++eye) {
      auto& v = handsView[eye];
      v.pose = s.eyeViews[eye].pose;
      v.fov  = s.eyeViews[eye].fov;
      v.subImage.swapchain = s.handsSwapchain[eye];
      v.subImage.imageRect = {{0,0},{int32_t(Impl::HandsSize),int32_t(Impl::HandsSize)}};
      }
    handsLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT; // premultiplied
    handsLayer.space      = s.localSpace;
    handsLayer.viewCount  = 2;
    handsLayer.views      = handsView;
    addLayer(handsLayer);
    }

  XrFrameEndInfo end = {XR_TYPE_FRAME_END_INFO};
  end.displayTime          = s.frame.predictedDisplayTime;
  end.environmentBlendMode = alphaRoom ? XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND : s.opaqueBlend;
  end.layerCount           = s.frame.shouldRender && isVisible() ? layerCount : 0;
  end.layers               = layers.data();
  {
  const uint64_t t0 = xrUs();
  const bool ended = s.sessionResult(xrEndFrame(s.session, &end), "xrEndFrame");
  tEndFrame += xrUs()-t0;
  if(s.traceFrameTiming) {
    const char* gameLayer = "none";
    if(!ended)
      gameLayer = "rejected";
    else if(end.layerCount!=0 && hasImage)
      gameLayer = warp ? "projection" : "quad";
    Log::i("XR frame timing targetNs=",end.displayTime," displayMs=",end.displayTime/1000000,
           " periodNs=",s.frame.predictedDisplayPeriod," refreshHz=",s.refreshRate," gameLayer=",gameLayer,
           " layers=",end.layerCount," requestedWarp=",int(rendered && s.spaceWarpWanted),
           " skip=",int(ended && end.layerCount!=0 && warp && s.skipExtrapolation));
    }
  if(s.traceSurface && spill!=nullptr) {
    const auto& p=spill->pose.position;
    const auto& q=spill->pose.orientation;
    Log::i("XR surface layer submitted=",int(ended && end.layerCount!=0)," time=",end.displayTime,
           " position=",p.x,",",p.y,",",p.z," orientation=",q.x,",",q.y,",",q.z,",",q.w,
           " size=",spill->size.width,"x",spill->size.height," blend=",s.hasAlphaBlend ? "ONE/ONE" : "premultiplied");
    }
  if(ended && end.layerCount!=0 && hasImage)
    s.skipExtrapolation = false;
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
  if(s.vrActive) {
    // VR: the runtime's recommended eye image, within a GPU budget, shaped like the field of view
    if(s.vrRecommended[0]==0) {
      XrViewConfigurationView cv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
      uint32_t count = 0;
      const bool known = XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                                         2, &count, cv)) && count>0 && cv[0].recommendedImageRectWidth>0;
      s.vrRecommended[0] = known ? cv[0].recommendedImageRectWidth  : 1536u;
      s.vrRecommended[1] = known ? cv[0].recommendedImageRectHeight : 1536u;
      }
    const auto& f      = s.eyeViews[0].fov;
    const float tanW   = std::tan(f.angleRight)-std::tan(f.angleLeft);
    const float tanH   = std::tan(f.angleUp)-std::tan(f.angleDown);
    const float aspect = tanW>0.f && tanH>0.f ? std::clamp(tanW/tanH, 0.5f, 2.f) : 1.f;
    const float pixels = std::min(float(s.vrRecommended[0])*float(s.vrRecommended[1]), float(Impl::VrEyePixels));
    auto multiple = [](float x,int n) { return std::max(n,int(std::round(x/float(n)))*n); };
    const int w = std::min(int(s.maxSwapW), multiple(std::sqrt(pixels*aspect), 16));
    const int h = std::min(int(s.maxSwapH), multiple(float(w)/aspect, 4));
    if(std::abs(float(w)/float(s.width)-1.f)<0.04f && std::abs(float(h)/float(s.height)-1.f)<0.04f)
      return resolution();
    return Size(w,h);
    }
  const float refHalf = s.verticalFov*float(M_PI)/360.f;
  const float refX = 2.f*std::atan((16.f/9.f)*std::tan(refHalf));
  // The image covers the eye's frame on the window plane (larger than the window when curved)
  float frameW = 0.f, frameH = 0.f;
  for(auto& r:s.eyeRect) {
    frameW = std::max(frameW,r[1]-r[0]);
    frameH = std::max(frameH,r[3]-r[2]);
    }
  if(!(frameW>0.f && frameH>0.f)) {
    frameW = s.windowWidth;
    frameH = s.windowHeight();
    }
  // Measure angular density across at most a 16:9 / 9:16 central chord so very wide (up to 32:9)
  // or very tall (up to 9:32) windows keep crisp central pixel density instead of dropping from
  // wide-angle foreshortening at the edges.
  float ppm = 0.f;
  float minZ = 1e9f;
  for(int i=0; i<2; ++i) {
    V3 e = s.eyeInWindow[i];
    e.z = std::max(e.z, 0.05f);
    minZ = std::min(minZ, e.z);
    const auto& r = s.eyeRect[size_t(i)];
    const float rw = r[1]-r[0], rh = r[3]-r[2];
    if(!(rw>0.f && rh>0.f))
      continue;
    const float cw = std::min(rw, rh*(16.f/9.f));
    const float ch = std::min(rh, rw*(16.f/9.f));
    const float cx = std::clamp(e.x, r[0]+cw*0.5f, r[1]-cw*0.5f);
    const float cy = std::clamp(e.y, r[2]+ch*0.5f, r[3]-ch*0.5f);
    const float angX = std::atan((cx+cw*0.5f-e.x)/e.z) - std::atan((cx-cw*0.5f-e.x)/e.z);
    const float angY = std::atan((cy+ch*0.5f-e.y)/e.z) - std::atan((cy-ch*0.5f-e.y)/e.z);
    ppm = std::max(ppm, 1600.f*angX/(refX*cw));
    ppm = std::max(ppm, 900.f*angY/(2.f*refHalf*ch));
    }
  if(s.scalePicture) {
    // In Scale picture mode, a very wide or tall window still fits the entire game FOV on its
    // shorter axis. Keep that axis reasonably sharp (up to ~22 ppd, bounded by a 1600x900 pixel
    // count) so slim ultrawide/portrait shapes do not become blurry.
    const float shortSide = std::min(frameW, frameH);
    const float floorPpm  = std::min({640.f/shortSide, 1260.f/minZ,
                                      std::sqrt(float(1600u*900u)/(frameW*frameH))});
    ppm = std::max(ppm, floorPpm);
    }
  if(s.isCurved()) {
    // The frame around a curve includes corners the mask hides: spend at most 30% more
    // pixels than the flat window would, so curving costs about the same GPU time.
    const float w = s.windowWidth, h = s.windowHeight();
    const float cw = std::min(w, h*(16.f/9.f)), ch = std::min(h, w*(16.f/9.f));
    float flatPpm = 0.f;
    for(int i=0; i<2; ++i) {
      V3 e = s.eyeInWindow[i];
      e.z = std::max(e.z, 0.05f);
      const float cx = std::clamp(e.x, -w*0.5f+cw*0.5f, w*0.5f-cw*0.5f);
      const float cy = std::clamp(e.y, -h*0.5f+ch*0.5f, h*0.5f-ch*0.5f);
      const float angX = std::atan((cx+cw*0.5f-e.x)/e.z) - std::atan((cx-cw*0.5f-e.x)/e.z);
      const float angY = std::atan((cy+ch*0.5f-e.y)/e.z) - std::atan((cy-ch*0.5f-e.y)/e.z);
      flatPpm = std::max(flatPpm, 1600.f*angX/(refX*cw));
      flatPpm = std::max(flatPpm, 900.f*angY/(2.f*refHalf*ch));
      }
    if(s.scalePicture) {
      const float shortSide = std::min(w, h);
      const float floorPpm  = std::min({640.f/shortSide, 1260.f/minZ,
                                        std::sqrt(float(1600u*900u)/(w*h))});
      flatPpm = std::max(flatPpm, floorPpm);
      }
    const float maxPixels = 1.3f*(w*flatPpm)*(h*flatPpm);
    const float curPixels = (frameW*ppm)*(frameH*ppm);
    if(curPixels>maxPixels)
      ppm *= std::sqrt(maxPixels/curPixels);
    }
  // Cap total eye pixels to the same GPU budget as 2048x1536 while allowing wide/tall
  // shapes to use up to maxSwapW / maxSwapH along their long axis.
  const float pixelCapPpm = std::sqrt(float(Impl::MaxEyePixels)/(frameW*frameH));
  ppm = std::min({ppm, pixelCapPpm, float(s.maxSwapW)/frameW, float(s.maxSwapH)/frameH});
  auto multiple = [](float x,int n) { return std::max(n,int(std::round(x/float(n)))*n); };
  int w = std::min(int(s.maxSwapW), multiple(frameW*ppm, 16));
  int h = std::min(int(s.maxSwapH), multiple(float(w)*frameH/frameW, 4));
  if(uint64_t(w)*uint64_t(h)>uint64_t(Impl::MaxEyePixels)) {
    const float k = std::sqrt(float(Impl::MaxEyePixels)/(float(w)*float(h)));
    w = std::max(16, int(float(w)*k/16.f)*16);
    h = std::max(4,  int(float(h)*k/4.f)*4);
    }
  const float aspect = frameW/frameH;
  if(std::abs(float(s.width)/float(s.height)/aspect-1.f)<0.02f &&
     std::abs(float(w)/float(s.width)-1.f)<0.08f && std::abs(float(h)/float(s.height)-1.f)<0.08f)
    return resolution();
  return Size(w,h);
  }

void XrWindow::setResolution(Size size) {
  auto& s = *impl;
  uint32_t nextW = uint32_t(std::max(1, size.w));
  uint32_t nextH = uint32_t(std::max(1, size.h));
  if((nextW>s.capacityW || nextH>s.capacityH) && !s.resizeSwapchains(nextW, nextH)) {
    s.maxSwapW = s.capacityW;
    s.maxSwapH = s.capacityH;
    }
  nextW = std::min(nextW, s.capacityW);
  nextH = std::min(nextH, s.capacityH);
  if(s.width!=nextW || s.height!=nextH)
    s.skipExtrapolation = true;
  s.width = nextW;
  s.height = nextH;
  if(!s.fullMotionExtent) {
    s.mvWidth = std::max(s.width/4u,1u);
    s.mvHeight = std::max(s.height/4u,1u);
    }
  }

float XrWindow::densityUiScale() const {
  const auto& s = *impl;
  if(!s.eyesValid)
    return uiScale();
  if(s.vrActive && s.width>0) {
    // The VR panel: interface elements about as large to the eye as in the window at its sweet spot
    const float panelDeg = 2.f*std::atan(Impl::VrPanelHalf*float(s.height)/float(s.width))*180.f/float(M_PI);
    const float scale    = uiScale()*(float(s.height)/900.f)*(s.verticalFov/panelDeg);
    return std::clamp(std::round(scale*8.f)/8.f,0.5f,3.f);
    }
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

bool XrWindow::isVrMode() const {
  return impl->vrMode;
  }

void XrWindow::setVrMode(bool vr) {
  if(vr!=impl->vrMode)
    impl->toggleVrMode("request");
  }

void XrWindow::Impl::toggleVrMode(const char* source) {
  vrMode = !vrMode;
  vrToggleAfter = frame.predictedDisplayTime + 1000000000;
  // The Video menu shows and sets the same choice
  Gothic::settingsSetI("INTERNAL", "xrVrMode", vrMode ? 1 : 0);
  Gothic::flushSettings();
  Log::i("OpenXR: VR mode ", vrMode ? "on" : "off", " (", source, ")");
  }

void XrWindow::Impl::updateVrToggles() {
  const XrTime now = frame.predictedDisplayTime;
  auto state = [&](XrAction action, XrPath hand, auto st) {
    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action        = action;
    gi.subactionPath = hand;
    XrResult r = XR_ERROR_HANDLE_INVALID;
    if constexpr(std::is_same_v<decltype(st),XrActionStateBoolean>)
      r = xrGetActionStateBoolean(session, &gi, &st);
    else if constexpr(std::is_same_v<decltype(st),XrActionStateFloat>)
      r = xrGetActionStateFloat(session, &gi, &st);
    else
      r = xrGetActionStateVector2f(session, &gi, &st);
    if(r!=XR_SUCCESS)
      st.isActive = XR_FALSE;
    return st;
    };
  // Double thumb tap (hand tracking microgesture): two taps of either hand within 0.4 s
  for(const auto& h:hands) {
    if(aThumbTap==XR_NULL_HANDLE || h.path==XR_NULL_PATH)
      continue;
    const auto tap = state(aThumbTap, h.path, XrActionStateBoolean{XR_TYPE_ACTION_STATE_BOOLEAN});
    if(!tap.isActive || !tap.currentState || !tap.changedSinceLastSync)
      continue;
    if(vrLastTap!=0 && now-vrLastTap<400000000 && now>=vrToggleAfter) {
      vrLastTap = 0;
      toggleVrMode("double thumb tap");
      } else {
      vrLastTap = now;
      }
    }
  // Both controller grips held for a second: in VR they grab nothing and no game action uses them
  bool held = grabbedBy<0 && aGrab!=XR_NULL_HANDLE && aStick!=XR_NULL_HANDLE;
  for(const auto& h:hands) {
    if(!held || h.path==XR_NULL_PATH) {
      held = false;
      break;
      }
    const auto stick = state(aStick, h.path, XrActionStateVector2f{XR_TYPE_ACTION_STATE_VECTOR2F}); // controllers only
    const auto grip  = state(aGrab,  h.path, XrActionStateFloat{XR_TYPE_ACTION_STATE_FLOAT});
    held = stick.isActive && grip.isActive && grip.currentState>0.75f;
    }
  if(!held) {
    vrGripSince   = 0;
    vrGripLatched = false;
    }
  else if(vrGripSince==0) {
    vrGripSince = now;
    }
  else if(!vrGripLatched && now-vrGripSince>=1000000000) {
    vrGripLatched = true;
    toggleVrMode("both grips held");
    }
  }

bool XrWindow::updateVrView(bool allowed, float heroYaw, Tempest::Vec3& spin, Tempest::Vec3& offset) {
  auto& s = *impl;
  s.vrMode = Gothic::settingsGetI("INTERNAL","xrVrMode")!=0;
  const bool active = s.vrMode && allowed && s.headValid;
  if(active!=s.vrActive) {
    // Nothing of the room shows in VR: pause the passthrough feed meanwhile
    if(s.passthrough!=XR_NULL_HANDLE && !s.immersive)
      ok(active ? s.xrPausePassthrough(s.passthrough) : s.xrStartPassthrough(s.passthrough), "switch passthrough");
    if(active && s.grabbed==Grip::ItemHandle)
      s.saveItemOffset();
    if(active) {
      s.grabbedBy = -1;
      s.grabbed   = Grip::None;
      }
    s.vrActive          = active;
    s.vrRefValid        = false;
    s.skipExtrapolation = true;
    Log::i("OpenXR: ", active ? "VR view" : "window view");
    }
  if(!active)
    return false;

  // Head yaw and pitch become the camera angles; roll and each eye's own pose stay in eyeView()
  const V3    f     = rotate(s.headOrientation, V3{0,0,-1});
  const float yaw   = std::atan2(-f.x, -f.z);
  const float pitch = std::asin(std::clamp(f.y, -1.f, 1.f));
  if(!s.vrRefValid) {
    // Where the wearer looks when the view starts is where the hero faces
    s.vrRefYaw   = yaw;
    s.vrRefPos   = s.headPos;
    s.vrRefValid = true;
    }
  s.vrBase    = mul(aroundY(yaw), aroundX(pitch));
  s.vrBasePos = s.headPos;

  // OpenXR turns left with growing yaw and looks up with growing pitch; the game camera turns
  // left with growing yaw too but looks down with growing pitch
  const float deg = 180.f/float(M_PI);
  spin = Tempest::Vec3(-pitch*deg, heroYaw + (yaw-s.vrRefYaw)*deg, 0.f);
  // Head motion in the body frame of the start (+x right, +y up, -z forward), within a metre
  V3 d = rotate(aroundY(-s.vrRefYaw), s.headPos - s.vrRefPos);
  if(const float h = std::hypot(d.x, d.z); h>1.f) {
    d.x /= h;
    d.z /= h;
    }
  // Game world for a body facing heroYaw: forward (cos, 0, sin), right (sin, 0, -cos), centimetres
  const float c = std::cos(heroYaw/deg), sn = std::sin(heroYaw/deg);
  offset = Tempest::Vec3(-d.z*c + d.x*sn, d.y, -d.z*sn - d.x*c)*100.f;
  return true;
  }

bool XrWindow::takePadInterruption(PadState& releaseSources) {
  releaseSources = impl->releasePad;
  return std::exchange(impl->padInterrupted,false);
  }

void XrWindow::setSceneShown(bool shown) {
  auto& s = *impl;
  if(s.sceneShown!=shown && s.curved)
    s.skipExtrapolation = true;
  s.sceneShown = shown;
  }

void XrWindow::setPointerMode(bool on) {
  impl->pointerMode = on;
  }

void XrWindow::noteGameInput() {
  impl->lastGameInput = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  }

bool XrWindow::isFocused() const {
  return AndroidApi::activityFocused() && impl->state==XR_SESSION_STATE_FOCUSED;
  }

bool XrWindow::isVisible() const {
  return AndroidApi::activityResumed() && impl->running &&
         (impl->state==XR_SESSION_STATE_VISIBLE || impl->state==XR_SESSION_STATE_FOCUSED);
  }

bool XrWindow::armTrackingActive() const {
  auto& s = *impl;
  return s.armTracking && s.state==XR_SESSION_STATE_FOCUSED && (s.armTarget[0].valid || s.armTarget[1].valid);
  }

bool XrWindow::handTargets(XrHandTargets& out) const {
  if(!armTrackingActive())
    return false;
  out.hand[0] = impl->armTarget[0];
  out.hand[1] = impl->armTarget[1];
  return true;
  }

bool XrWindow::headOrientation(Tempest::Vec4& out) const {
  auto& s = *impl;
  if(!s.armTracking || !s.armHeadValid)
    return false;
  // Relative to where the wearer looked when the VR view started: that direction is the hero's facing
  const XrQuaternionf q = mul(aroundY(-s.vrRefYaw), s.armHead);
  out = Vec4(q.x, q.y, q.z, q.w);
  return true;
  }

void XrWindow::setVrEyeAnchor(const Tempest::Vec3& eyeFromHead) {
  impl->vrEyeAnchor = V3{eyeFromHead.x, eyeFromHead.y, eyeFromHead.z};
  }

bool XrWindow::vrEyeAnchor(Tempest::Vec3& out) const {
  auto& s = *impl;
  if(!s.vrActive || !s.vrRefValid)
    return false;
  out = Tempest::Vec3(s.vrEyeAnchor.x, s.vrEyeAnchor.y, s.vrEyeAnchor.z);
  return true;
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
bool      XrWindow::waitFrame() { return false; }
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
Tempest::Rect XrWindow::uiViewport(uint8_t) const { return Tempest::Rect(); }
void      XrWindow::drawSurfaceLight(Renderer&, Tempest::Encoder<Tempest::CommandBuffer>&, bool) {}
void      XrWindow::prepareLightingRate(Tempest::Encoder<Tempest::CommandBuffer>&, uint8_t, uint8_t, uint32_t, uint32_t) {}
void      XrWindow::drawInventoryItem(Tempest::Encoder<Tempest::CommandBuffer>&, uint8_t, const Item*) {}
void      XrWindow::drawHands(Tempest::Encoder<Tempest::CommandBuffer>&, uint8_t, uint8_t) {}
#endif
bool      XrWindow::prepareSpaceWarp(bool, bool) { return false; }
Tempest::Attachment* XrWindow::motionTarget(uint8_t) { return nullptr; }
Tempest::ZBuffer*    XrWindow::depthTarget(uint8_t) { return nullptr; }
Tempest::Attachment* XrWindow::eyeTarget(uint8_t) { return nullptr; }
bool      XrWindow::eyeView(uint8_t, float, Camera::EyeView&) const { return false; }
bool      XrWindow::isVrMode() const { return false; }
void      XrWindow::setVrMode(bool) {}
bool      XrWindow::updateVrView(bool, float, Tempest::Vec3&, Tempest::Vec3&) { return false; }
void      XrWindow::setVerticalFov(float) {}
std::string XrWindow::timingReport() { return ""; }
PadState  XrWindow::pad() const { return {}; }
bool XrWindow::takePadInterruption(PadState&) { return false; }
bool      XrWindow::isFocused() const { return false; }
void      XrWindow::noteGameInput() {}
void      XrWindow::setSceneShown(bool) {}
void      XrWindow::setPointerMode(bool) {}
bool      XrWindow::isVisible() const { return false; }
bool      XrWindow::armTrackingActive() const { return false; }
bool      XrWindow::handTargets(XrHandTargets&) const { return false; }
bool      XrWindow::headOrientation(Tempest::Vec4&) const { return false; }
void      XrWindow::setVrEyeAnchor(const Tempest::Vec3&) {}
bool      XrWindow::vrEyeAnchor(Tempest::Vec3&) const { return false; }

#endif
