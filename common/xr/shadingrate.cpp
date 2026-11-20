#include "shadingrate.h"

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)
#include "commandline.h"
#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace Tempest;

constexpr VkImageUsageFlags RateUsage = VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

VkExtent2D ShadingRateMap::supported(VkInstance instance, VkPhysicalDevice gpu, const VkDeviceCreateInfo& ci) {
  bool dynamicRendering = false;
  for(auto p = static_cast<const VkBaseInStructure*>(ci.pNext); p!=nullptr; p=p->pNext) {
    if(p->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)
      dynamicRendering = reinterpret_cast<const VkPhysicalDeviceDynamicRenderingFeatures*>(p)->dynamicRendering;
    }
  if(!dynamicRendering)
    return {};
  uint32_t count = 0;
  if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,nullptr)!=VK_SUCCESS || count==0)
    return {};
  std::vector<VkExtensionProperties> extensions(count);
  if(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,extensions.data())!=VK_SUCCESS ||
     std::none_of(extensions.begin(),extensions.end(),[](const auto& e){
       return std::strcmp(e.extensionName,VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)==0;
       }))
    return {};

  VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
  VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,&fsr};
  vkGetPhysicalDeviceFeatures2(gpu,&features);
  VkPhysicalDeviceFragmentShadingRatePropertiesKHR rates = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,&rates};
  vkGetPhysicalDeviceProperties2(gpu,&properties);
  if(properties.properties.apiVersion<VK_API_VERSION_1_2 || !fsr.pipelineFragmentShadingRate || !fsr.attachmentFragmentShadingRate)
    return {};
  VkImageFormatProperties format = {};
  if(vkGetPhysicalDeviceImageFormatProperties(gpu,VK_FORMAT_R8_UINT,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
                                             RateUsage,0,&format)!=VK_SUCCESS || !(format.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
    return {};
  const auto getRates = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(
                            vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceFragmentShadingRatesKHR"));
  count = 0;
  if(!getRates || getRates(gpu,&count,nullptr)!=VK_SUCCESS || count==0)
    return {};
  std::vector<VkPhysicalDeviceFragmentShadingRateKHR> sizes(count,{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
  if(getRates(gpu,&count,sizes.data())!=VK_SUCCESS)
    return {};
  for(auto& size:sizes)
    if(size.fragmentSize.width==2 && size.fragmentSize.height==2 && (size.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
      const uint32_t side = std::max(rates.minFragmentShadingRateAttachmentTexelSize.width,rates.minFragmentShadingRateAttachmentTexelSize.height);
      if(side<=rates.maxFragmentShadingRateAttachmentTexelSize.width && side<=rates.maxFragmentShadingRateAttachmentTexelSize.height)
        return {side,side};
      }
  return {};
  }

ShadingRateMap::~ShadingRateMap() {
  release();
  }

void ShadingRateMap::release() {
  if(device==VK_NULL_HANDLE)
    return;
  if(mapped)
    vkUnmapMemory(device,bufferMemory);
  vkDestroyBuffer(device,rate.source,nullptr);
  vkDestroyImageView(device,rate.view,nullptr);
  vkDestroyImage(device,rate.image,nullptr);
  vkFreeMemory(device,bufferMemory,nullptr);
  vkFreeMemory(device,imageMemory,nullptr);
  imageMemory = bufferMemory = VK_NULL_HANDLE;
  device = VK_NULL_HANDLE;
  mapped = nullptr;
  rate = {};
  }

bool ShadingRateMap::allocate(const VulkanNativeContext& vk, VkExtent2D extent, VkExtent2D texel) {
  VkImageFormatProperties format = {};
  if(vkGetPhysicalDeviceImageFormatProperties(vk.physicalDevice,VK_FORMAT_R8_UINT,VK_IMAGE_TYPE_2D,
                                              VK_IMAGE_TILING_OPTIMAL,RateUsage,0,&format)!=VK_SUCCESS ||
     extent.width>format.maxExtent.width || extent.height>format.maxExtent.height)
    return false;
  device = vk.device;
  VkPhysicalDeviceMemoryProperties props = {};
  vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice,&props);
  auto alloc = [&](VkMemoryRequirements req, VkMemoryPropertyFlags flags, VkDeviceMemory& out) {
    for(uint32_t i=0;i<props.memoryTypeCount;++i) {
      if((req.memoryTypeBits & (1u<<i))==0 || (props.memoryTypes[i].propertyFlags & flags)!=flags)
        continue;
      VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      info.allocationSize = req.size;
      info.memoryTypeIndex = i;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      if(vkAllocateMemory(device,&info,nullptr,&memory)==VK_SUCCESS) {
        out = memory;
        return true;
        }
      }
    return false;
    };
  VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType   = VK_IMAGE_TYPE_2D;
  image.format      = VK_FORMAT_R8_UINT;
  image.extent      = {extent.width,extent.height,1};
  image.mipLevels   = image.arrayLayers = 1;
  image.samples     = VK_SAMPLE_COUNT_1_BIT;
  image.tiling      = VK_IMAGE_TILING_OPTIMAL;
  image.usage       = RateUsage;
  VkImage img = VK_NULL_HANDLE;
  if(vkCreateImage(device,&image,nullptr,&img)!=VK_SUCCESS)
    return false;
  rate.image = img;
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device,rate.image,&req);
  if(!alloc(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,imageMemory) || vkBindImageMemory(device,rate.image,imageMemory,0)!=VK_SUCCESS)
    return false;
  VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = rate.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = image.format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  VkImageView imgView = VK_NULL_HANDLE;
  if(vkCreateImageView(device,&view,nullptr,&imgView)!=VK_SUCCESS)
    return false;
  rate.view = imgView;
  VkBufferCreateInfo buf = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buf.size  = VkDeviceSize(extent.width)*extent.height;
  buf.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer buffer = VK_NULL_HANDLE;
  if(vkCreateBuffer(device,&buf,nullptr,&buffer)!=VK_SUCCESS)
    return false;
  rate.source = buffer;
  vkGetBufferMemoryRequirements(device,rate.source,&req);
  coherent = alloc(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,bufferMemory);
  if((!coherent && !alloc(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,bufferMemory)) ||
     vkBindBufferMemory(device,rate.source,bufferMemory,0)!=VK_SUCCESS)
    return false;
  void* ptr = nullptr;
  if(vkMapMemory(device,bufferMemory,0,VK_WHOLE_SIZE,0,&ptr)!=VK_SUCCESS)
    return false;
  mapped = static_cast<uint8_t*>(ptr);
  rate.extent = extent;
  rate.texel = texel;
  return true;
  }

bool ShadingRateMap::prepare(const VulkanNativeContext& vk, VkExtent2D texel, uint32_t w, uint32_t h,
                             Vec2 window, Vec3 eye, Vec3 forward) {
  const VkExtent2D extent = {(w-1)/texel.width+1,(h-1)/texel.height+1};
  const bool resized = rate.extent.width!=extent.width || rate.extent.height!=extent.height;
  if(resized) {
    release(); // The caller has waited for this eye slot's fence.
    if(!allocate(vk,extent,texel)) {
      release();
      return false;
      }
    }
  const float dx = window.x*float(texel.width)/float(w), dy = window.y*float(texel.height)/float(h);
  constexpr float cos25 = 0.9063078f;
  // In a tile of radius r, dot(ray,forward) can increase by at most r and |ray|
  // can decrease by at most r. Only coarsen tiles entirely outside the full-rate cone.
  const float margin = (1.f+cos25)*0.5f*std::sqrt(dx*dx+dy*dy);
  const float x0 = (dx-window.x)*0.5f-eye.x, y0 = (window.y-dy)*0.5f-eye.y;
  for(uint32_t j=0;j<extent.height;++j) {
    const float y = y0-float(j)*dy;
    const float yz = y*y+eye.z*eye.z, projYZ = y*forward.y-eye.z*forward.z+margin;
    for(uint32_t i=0;i<extent.width;++i) {
      const float x = x0+float(i)*dx, proj = x*forward.x+projYZ;
      const bool outside = proj<0.f || proj*proj<cos25*cos25*(x*x+yz);
      mapped[size_t(j)*extent.width+i] = outside && i<w/texel.width && j<h/texel.height ? 5 : 0;
      }
    }
  if(resized && CommandLine::inst().isGpuProfile()) {
    const size_t tiles = size_t(extent.width)*extent.height;
    Log::i("OpenXR: lighting rate map ",extent.width,"x",extent.height," coarse tiles ",
           uint32_t(std::count(mapped,mapped+tiles,5)),"/",uint32_t(tiles));
    }
  if(!coherent) {
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = bufferMemory;
    range.size = VK_WHOLE_SIZE;
    if(vkFlushMappedMemoryRanges(device,1,&range)!=VK_SUCCESS)
      return false;
    }
  return true;
  }
#endif
