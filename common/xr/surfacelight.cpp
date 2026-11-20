#include "surfacelight.h"

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)
#include <Tempest/Device>
#include <Tempest/Except>
#include <Tempest/Log>
#include <Tempest/VulkanInterop>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <new>
#include <system_error>
#include <utility>

using namespace Tempest;

bool SurfaceLight::create(Device& device, XrSession session, int64_t format) {
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
  if(format==VK_FORMAT_R8G8B8A8_SRGB)
    info.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
  info.format = format;
  info.width = Width;
  info.height = Height;
  info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
  XrSwapchain candidate = XR_NULL_HANDLE;
  if(XR_FAILED(xrCreateSwapchain(session,&info,&candidate)))
    return false;
  struct Guard {
    XrSwapchain chain;
    ~Guard() { if(chain!=XR_NULL_HANDLE) xrDestroySwapchain(chain); }
    } guard{candidate};

  uint32_t count = 0;
  if(XR_FAILED(xrEnumerateSwapchainImages(candidate,0,&count,nullptr)) || count==0)
    return false;
  std::vector<XrSwapchainImageVulkanKHR> native(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  if(XR_FAILED(xrEnumerateSwapchainImages(candidate,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(native.data()))) ||
     count==0 || count>native.size())
    return false;
  std::vector<Attachment> wrapped;
  wrapped.reserve(count);
  for(uint32_t i=0; i<count; ++i)
    wrapped.push_back(vulkanBorrowColorAttachment(device,native[i].image,Width,Height));
  images = std::move(wrapped);
  chain = std::exchange(guard.chain,XR_NULL_HANDLE);
  return true;
  }

Attachment* SurfaceLight::target(Device& device, XrSession session, int64_t format, std::mutex& queueLock, XrTime displayTime) {
  if(failed || displayTime<retryAt)
    return nullptr;
  if(reuse && valid) {
    reuse = false;
    return nullptr;
    }
  std::lock_guard<std::mutex> lock(queueLock);
  if(chain==XR_NULL_HANDLE) {
    try { create(device,session,format); }
    catch(const std::bad_alloc&) {}
    catch(const std::system_error& e) {
      if(e.code()!=GraphicsErrc::OutOfVideoMemory && e.code()!=GraphicsErrc::OutOfHostMemory)
        throw;
      }
    if(chain==XR_NULL_HANDLE) {
      retryAt = displayTime+3000000000LL;
      Log::e("OpenXR: could not allocate surface light swapchain");
      return nullptr;
      }
    }
  if(acquired<0) {
    const XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t index = 0;
    if(XR_FAILED(xrAcquireSwapchainImage(chain,&acquire,&index)))
      return nullptr;
    acquired = int32_t(index);
    }
  if(!waited) {
    const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,0};
    const XrResult result = xrWaitSwapchainImage(chain,&wait);
    if(result==XR_TIMEOUT_EXPIRED)
      return nullptr;
    if(result!=XR_SUCCESS || uint32_t(acquired)>=images.size()) {
      failed = true;
      valid = false;
      Log::e("OpenXR: surface light image unavailable (",int(result),")");
      return nullptr;
      }
    waited = true;
    }
  cancelled = false;
  return &images[size_t(acquired)];
  }

void SurfaceLight::finish(bool submitted) {
  if(failed || acquired<0)
    return;
  if(!waited) {
    // A probe can be cancelled after a nonblocking wait times out. Return the
    // acquired image as soon as the runtime makes it available.
    if(!cancelled)
      return;
    const XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,0};
    const XrResult result = xrWaitSwapchainImage(chain,&wait);
    if(result==XR_TIMEOUT_EXPIRED)
      return;
    if(result!=XR_SUCCESS) {
      failed = true;
      Log::e("OpenXR: surface light wait failed (",int(result),")");
      return;
      }
    }
  const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  const XrResult result = xrReleaseSwapchainImage(chain,&release);
  failed = XR_FAILED(result);
  valid = !failed && recordedImage && submitted;
  if(failed)
    Log::e("OpenXR: surface light release failed (",int(result),")");
  reuse = valid;
  acquired = -1;
  waited = false;
  cancelled = false;
  recordedImage = false;
  }

const XrCompositionLayerQuad* SurfaceLight::layer(XrSpace space, const XrPosef& pose, XrExtent2Df size, bool additive) {
  if(!valid)
    return nullptr;
  blend.srcFactorColor = XR_BLEND_FACTOR_ONE_FB;
  blend.dstFactorColor = XR_BLEND_FACTOR_ONE_FB;
  blend.srcFactorAlpha = XR_BLEND_FACTOR_ZERO_FB;
  blend.dstFactorAlpha = XR_BLEND_FACTOR_ONE_FB;
  quad.next = additive ? &blend : nullptr;
  quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  quad.space = space;
  quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  quad.subImage.swapchain = chain;
  quad.subImage.imageRect = {{0,0},{int32_t(Width),int32_t(Height)}};
  quad.pose = pose;
  quad.size = size;
  return &quad;
  }

void SurfaceLight::invalidate() {
  valid = false;
  reuse = false;
  recordedImage = false;
  cancelled = acquired>=0;
  }

void SurfaceLight::reset() {
  images.clear();
  if(chain!=XR_NULL_HANDLE)
    xrDestroySwapchain(chain);
  chain = XR_NULL_HANDLE;
  acquired = -1;
  waited = false;
  failed = false;
  retryAt = 0;
  invalidate();
  }
#endif
