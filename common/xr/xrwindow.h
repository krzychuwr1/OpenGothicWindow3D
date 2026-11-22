#pragma once

#include <Tempest/Device>
#include <Tempest/Attachment>
#include <Tempest/Size>
#include <Tempest/Vec>

#include <memory>
#include <string>

#include "gamepadinput.h"
#include "camera.h"

// Presents the game as a floating window in the user's room (passthrough around it)
// through OpenXR. Only available in Android builds with OPENGOTHIC_XR; elsewhere
// XrWindow::inst() is always null and the game uses its normal window.
class Renderer;
class Item;

// One tracked hand (XR_EXT_hand_tracking) relative to the HEAD (the VIEW reference space), in metres.
// Axes are OpenXR's: +X right, +Y up, -Z forward (where the eyes look).
// The orientation is the wrist joint's: -Z along the hand towards the fingers, +Y out of the back of
// the hand, X = Y x Z (towards the little finger on the right hand, towards the thumb on the left).
struct XrHandTarget {
  bool          valid = false;
  Tempest::Vec3 wrist;
  Tempest::Vec4 orientation = {0,0,0,1}; // quaternion x,y,z,w
  Tempest::Vec3 palm;
  Tempest::Vec3 knuckle;                 // index finger root (proximal joint)
  Tempest::Vec3 thumbBase;               // thumb root (metacarpal joint)
  float         curl = 0.f;              // mean bend of the four fingers: 0 open, 1 fist
  };

struct XrHandTargets {
  XrHandTarget hand[2]; // 0 left, 1 right
  };

class XrWindow final {
  public:
    ~XrWindow();

    // Must run before the Vulkan API is created: OpenXR chooses the instance and device.
    // Returns false (and leaves the normal window path) when no OpenXR runtime is present.
    static bool       preInit();
    static XrWindow*  inst();

    void              attach(Tempest::Device& device);

    // Game image resolution of the window, in pixels.
    Tempest::Size     resolution() const;
    Tempest::Size     desiredResolution() const;
    void              setResolution(Tempest::Size size);
    float             densityUiScale() const;
    float             uiScale() const { return 1.25f; }
    // Game camera field of view; places the window where it shows the whole view.
    void              setVerticalFov(float degrees);
    // Time spent waiting inside submit() since the previous report
    static std::string timingReport();

    // Poll session events. A focus interruption suppresses input for this update, even if focus returned.
    bool              pollInputFocus(bool nativeFocusLost);
    // Waits for the next display frame and refreshes XR input for its predicted display time.
    // Returns true when the application should tick simulation and then call beginFrame().
    bool              waitFrame();
    // Begins rendering the waited frame and locates eye views for it.
    // Returns true when the application should render and then call submit().
    bool              beginFrame();
    int64_t           nominalFramePeriod() const;
    int64_t           predictedDisplayTime() const; // nanoseconds on the OpenXR clock
    // Render target of one eye for this frame (the window layer's swapchain image).
    Tempest::Attachment* eyeTarget(uint8_t eye);

    // Application SpaceWarp (XR_FB_space_warp): when used for a frame, each eye also provides
    // motion vectors and depth, the window is submitted as a projection layer and the runtime
    // renders every other display frame itself.
    bool              isSpaceWarpSupported() const;
#ifdef OPENGOTHIC_XR
    bool              isFoveationSupported() const;
    bool              isWindowFilterSupported() const;
    bool              isSurfaceLightSupported() const;
    void              drawWindowEdges(Tempest::Encoder<Tempest::CommandBuffer>& cmd, bool motion = false);
    // Curved window: the part of an eye image that holds the interface (the flat window
    // rectangle inside the larger frame that contains the curve); empty when flat
    Tempest::Rect     uiViewport(uint8_t eye) const;
    void              drawSurfaceLight(Renderer& renderer, Tempest::Encoder<Tempest::CommandBuffer>& cmd, bool sceneReady);
    void              prepareLightingRate(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t slot,
                                          uint8_t eye, uint32_t width, uint32_t height);
#endif
    // Returns whether motion can use the previous view across this frame boundary.
    bool              prepareSpaceWarp(bool use, bool previousViewValid);
    Tempest::Attachment* motionTarget(uint8_t eye);
    Tempest::ZBuffer*    depthTarget(uint8_t eye);
    // Ends the frame; without a rendered image only passthrough shows.
    bool              submit(bool rendered); // true if a SpaceWarp pair was submitted

    // Camera of one eye looking through the window into the game world, for this frame's
    // predicted eye positions. False while the eyes are not tracked.
    bool              eyeView(uint8_t eye, float verticalFovDeg, Camera::EyeView& out) const;

    // VR mode ("step through the portal"): a first-person view of the game world filling the
    // headset instead of the window. Chosen by the wearer (chrome button, Video menu, V/F12 key,
    // double thumb tap, both grips held); shown only while the game allows it.
    bool              isVrMode() const;
    void              setVrMode(bool vr);
    // Once per game update: whether the game allows the VR view now (otherwise the window shows).
    // True when this frame needs the VR camera pose: it renders VR, or the passage between window
    // and VR is under way (see vrTransition()). Then `spin` holds the camera angles in degrees
    // (pitch down, yaw, 0) and `offset` the head motion since the view started, in game units in
    // world space, both for a body facing heroYaw (degrees).
    bool              updateVrView(bool allowed, float heroYaw, Tempest::Vec3& spin, Tempest::Vec3& offset);
    // How far the camera is from the game camera (0) to the VR pose (1), eased: between the two
    // the window flies to the eyes and grows (or shrinks back) while the camera glides along
    float             vrTransition() const;

    // Controller state of the XR controllers (empty while the window is grabbed or the app is unfocused).
    PadState          pad() const;
    // Previous XR controls still eligible to generate an ordinary release.
    bool              takePadInterruption(PadState& releaseSources);
    bool              isFocused() const;
    // Keyboard, mouse or pad input for the game: the window controls ignore pointing and
    // pinching until it has been quiet for a moment, so playing never grabs them by accident
    void              noteGameInput();
    // The game shows its 3D scene (not the main menu, a loading screen or a video): only then
    // can the window be curved; flat pictures are shown on a flat window
    void              setSceneShown(bool shown);
    // The game is paused or a game menu is open: controller rays and triggers work the window
    // controls (move, resize, the buttons below) and the triggers stop reaching the game
    void              setPointerMode(bool on);
#ifdef OPENGOTHIC_XR
    // Selected inventory item rendered as a true 3D stereo model outside the window
    void              drawInventoryItem(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t eye, const Item* item);
    // Ghost hands and pointer rays, drawn into their own layer over everything (off, auto
    // or always, from the hand button below the window). slot: the eye's frame slot.
    void              drawHands(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t eye, uint8_t slot);
#endif
    // The session shows frames (headset worn and the app in front, possibly under a system menu)
    bool              isVisible() const;

    // The hero's arms follow the user's hands: hand tracking is available, the hands (not
    // controllers) are tracked and the xrArmTracking setting is on
    bool              armTrackingActive() const;
    // This frame's hands relative to the head; false while arm tracking is inactive
    bool              handTargets(XrHandTargets& out) const;
    // Head (VIEW space) orientation relative to the room (LOCAL space), quaternion x,y,z,w;
    // located with the hands, so only while arm tracking is active
    bool              headOrientation(Tempest::Vec4& out) const;
    // VR view: where the eyes are relative to the hero's head bone (game world axes, centimetres),
    // set by the VR camera each frame; vrEyeAnchor() is false when the VR view is not shown
    void              setVrEyeAnchor(const Tempest::Vec3& eyeFromHead);
    bool              vrEyeAnchor(Tempest::Vec3& out) const;

  private:
    XrWindow();
    struct Impl;
    std::unique_ptr<Impl> impl;
  };
