#pragma once

#include <Tempest/Device>
#include <Tempest/Attachment>
#include <Tempest/Size>

#include <memory>

#include "gamepadinput.h"
#include "camera.h"

// Presents the game as a floating window in the user's room (passthrough around it)
// through OpenXR. Only available in Android builds with OPENGOTHIC_XR; elsewhere
// XrWindow::inst() is always null and the game uses its normal window.
class XrWindow final {
  public:
    ~XrWindow();

    // Must run before the Vulkan API is created: OpenXR chooses the instance and device.
    // Returns false (and leaves the normal window path) when no OpenXR runtime is present.
    static bool       preInit();
    static XrWindow*  inst();
    static void       shutdown();

    void              attach(Tempest::Device& device);

    // Game image resolution of the window, in pixels.
    Tempest::Size     resolution() const;
    float             uiScale() const { return 1.25f; }
    // Game camera field of view; places the window where it shows the whole view.
    void              setVerticalFov(float degrees);

    // Processes runtime events and waits for the next display frame.
    // Returns true when the application should render and then call submit().
    bool              beginFrame();
    // Copies the eye images into the window layers and ends the frame.
    // Null images show passthrough only; a single image is shown to both eyes.
    void              submit(const Tempest::Attachment* left, const Tempest::Attachment* right = nullptr);

    // Camera of one eye looking through the window into the game world, for this frame's
    // predicted eye positions. False while the eyes are not tracked.
    bool              eyeView(uint8_t eye, float verticalFovDeg, Camera::EyeView& out) const;

    // Controller state of the XR controllers (empty while the window is grabbed or the app is unfocused).
    PadState          pad() const;
    bool              isFocused() const;

  private:
    XrWindow();
    struct Impl;
    std::unique_ptr<Impl> impl;
  };
