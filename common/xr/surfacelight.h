#pragma once

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)
#include <Tempest/Attachment>
#include <openxr/openxr.h>

#include <mutex>
#include <vector>

namespace Tempest { class Device; }

class SurfaceLight final {
  public:
    static constexpr uint32_t Width=32, Height=18;

    Tempest::Attachment* target(Tempest::Device& device, XrSession session, int64_t format,
                                std::mutex& queueLock, XrTime displayTime);
    void recorded(bool written) { recordedImage = written; }
    // Called under the graphics queue lock, after recorded commands have been submitted.
    void finish(bool submitted);
    const XrCompositionLayerQuad* layer(XrSpace space, const XrPosef& pose, XrExtent2Df size, bool additive);
    void invalidate();
    void reset(); // after the GPU is idle, before destroying the session

  private:
    bool create(Tempest::Device& device, XrSession session, int64_t format);
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<Tempest::Attachment> images;
    XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerAlphaBlendFB blend = {XR_TYPE_COMPOSITION_LAYER_ALPHA_BLEND_FB};
    XrTime retryAt = 0;
    int32_t acquired = -1;
    bool waited = false, cancelled = false, recordedImage = false, valid = false, reuse = false, failed = false;
  };
#endif
