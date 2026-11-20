#pragma once

#include <Tempest/Device>
#include <Tempest/Attachment>
#include <Tempest/Size>

#include <memory>
#include <string>

#include "gamepadinput.h"
#include "camera.h"

// Presents the game as a floating window in the user's room (passthrough around it)
// through OpenXR. Only available in Android builds with OPENGOTHIC_XR; elsewhere
// XrWindow::inst() is always null and the game uses its normal window.
class Renderer;

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
    // Waits for the next display frame.
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

    // Controller state of the XR controllers (empty while the window is grabbed or the app is unfocused).
    PadState          pad() const;
    // Previous XR controls still eligible to generate an ordinary release.
    bool              takePadInterruption(PadState& releaseSources);
    bool              isFocused() const;
    // The session shows frames (headset worn and the app in front, possibly under a system menu)
    bool              isVisible() const;

  private:
    XrWindow();
    struct Impl;
    std::unique_ptr<Impl> impl;
  };
