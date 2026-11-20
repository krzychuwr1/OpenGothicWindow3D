#pragma once

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)
#include <Tempest/VulkanInterop>
#include <Tempest/Vec>

class ShadingRateMap {
  public:
    ShadingRateMap() = default;
    ~ShadingRateMap();
    ShadingRateMap(const ShadingRateMap&) = delete;
    ShadingRateMap& operator=(const ShadingRateMap&) = delete;

    // Returns the supported texel size, or zero when the optional path cannot be used.
    static VkExtent2D supported(VkInstance instance, VkPhysicalDevice gpu, const VkDeviceCreateInfo& ci);
    bool prepare(const Tempest::VulkanNativeContext& vk, VkExtent2D texel, uint32_t w, uint32_t h,
                 Tempest::Vec2 window, Tempest::Vec3 eye, Tempest::Vec3 forward);
    Tempest::VulkanShadingRateAttachment rate;

  private:
    bool allocate(const Tempest::VulkanNativeContext& vk, VkExtent2D extent, VkExtent2D texel);
    void release();

    VkDevice       device = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE, bufferMemory = VK_NULL_HANDLE;
    uint8_t*       mapped = nullptr;
    bool           coherent = false;
  };
#endif
