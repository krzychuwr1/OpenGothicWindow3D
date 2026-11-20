#pragma once

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)
#include <openxr/openxr.h>

#include <memory>
#include <optional>

namespace Tempest { class Device; }

class EnvironmentDepth final {
  public:
    struct Surface {
      XrPosef     pose;            // LOCAL, +Z up and +Y towards the window
      XrExtent2Df size;
      XrVector2f  uvOffset;        // clipped part of the full light footprint
      XrVector2f  uvScale;
      float       drop;            // metres below the bottom of the window
      };

    EnvironmentDepth();
    ~EnvironmentDepth();
    // Call only after the extension is enabled and Android spatial permission is granted.
    bool initialize(XrInstance instance, XrSystemId system, XrSession session, Tempest::Device& device);
    void request(const XrPosef& window, XrExtent2Df size);
    void update(XrSpace local, XrTime displayTime); // once between begin/end while focused
    void cancel(); // invalidate on movement, focus loss, setting or reference-space changes
    void reset();  // before session or device teardown
    const std::optional<Surface>& surface() const { return fitted; }
    // True once after a probe ended without finding a surface (reported to the player)
    bool takeFailure() { const bool f = failed; failed = false; return f; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::optional<Surface> fitted;
    bool                   failed = false;
  };
#endif
