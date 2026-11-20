#include "layeredtargets.h"

#include <Tempest/VulkanInterop>
#include <Tempest/Log>

#include <mutex>

using namespace Tempest;

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)

static VkFormat vkFormat(TextureFormat f) {
  switch(f) {
    case TextureFormat::RGBA8:       return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R32U:        return VK_FORMAT_R32_UINT;
    case TextureFormat::RG16:        return VK_FORMAT_R16G16_UNORM;
    case TextureFormat::RGBA16F:     return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R11G11B10UF: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case TextureFormat::Depth16:     return VK_FORMAT_D16_UNORM;
    case TextureFormat::Depth24x8:   return VK_FORMAT_X8_D24_UNORM_PACK32;
    case TextureFormat::Depth32F:    return VK_FORMAT_D32_SFLOAT;
    default:                         return VK_FORMAT_UNDEFINED;
    }
  }

static bool isDepth(TextureFormat f) {
  return f==TextureFormat::Depth16 || f==TextureFormat::Depth24x8 || f==TextureFormat::Depth32F;
  }

// Creates a two-layer image and moves it into the layout Tempest expects of lent images
static bool create(const VulkanNativeContext& c, TextureFormat frm, uint32_t w, uint32_t h,
                   VkImage& img, VkDeviceMemory& mem) {
  const VkFormat f     = vkFormat(frm);
  const bool     depth = isDepth(frm);
  if(f==VK_FORMAT_UNDEFINED)
    return false;

  VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType     = VK_IMAGE_TYPE_2D;
  ci.format        = f;
  ci.extent        = {w, h, 1};
  ci.mipLevels     = 1;
  ci.arrayLayers   = 2;
  ci.samples       = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
  ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if(vkCreateImage(c.device, &ci, nullptr, &img)!=VK_SUCCESS)
    return false;

  VkMemoryRequirements rq = {};
  vkGetImageMemoryRequirements(c.device, img, &rq);
  VkPhysicalDeviceMemoryProperties mp = {};
  vkGetPhysicalDeviceMemoryProperties(c.physicalDevice, &mp);
  uint32_t type = uint32_t(-1);
  for(uint32_t i=0; i<mp.memoryTypeCount; ++i) {
    if((rq.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      type = i;
      break;
      }
    }
  VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize  = rq.size;
  ai.memoryTypeIndex = type;
  if(type==uint32_t(-1) || vkAllocateMemory(c.device, &ai, nullptr, &mem)!=VK_SUCCESS) {
    vkDestroyImage(c.device, img, nullptr);
    img = VK_NULL_HANDLE;
    return false;
    }
  vkBindImageMemory(c.device, img, mem, 0);

  VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.queueFamilyIndex = c.queueFamily;
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCreateCommandPool(c.device, &pi, nullptr, &pool);
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool        = pool;
  cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(c.device, &cai, &cb);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask       = 0;
  b.dstAccessMask       = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = img;
  b.subresourceRange    = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 2};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &b);
  vkEndCommandBuffer(cb);

  VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vkCreateFence(c.device, &fi, nullptr, &fence);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers    = &cb;
  {
  std::lock_guard<std::mutex> guard(*c.queueLock);
  vkQueueSubmit(c.queue, 1, &si, fence);
  }
  vkWaitForFences(c.device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(c.device, fence, nullptr);
  vkDestroyCommandPool(c.device, pool, nullptr);
  return true;
  }

LayeredTargets::~LayeredTargets() {
  release();
  }

void LayeredTargets::release() {
  if(dev==nullptr)
    return;
  dev->waitIdle();
  diffuse = Attachment();
  normal  = Attachment();
  depth   = ZBuffer();
  auto c = vulkanNativeContext(*dev);
  for(auto& i:img) {
    if(i.image!=0)
      vkDestroyImage(c.device, VkImage(i.image), nullptr);
    if(i.memory!=0)
      vkFreeMemory(c.device, VkDeviceMemory(i.memory), nullptr);
    i = Img();
    }
  w = h = 0;
  }

bool LayeredTargets::resize(Device& device, uint32_t nw, uint32_t nh, TextureFormat d, TextureFormat n, TextureFormat z) {
  if(nw==w && nh==h && frm[0]==d && frm[1]==n && frm[2]==z && dev==&device)
    return true;
  release();
  dev = &device;
  auto c = vulkanNativeContext(device);
  const TextureFormat f[3] = {d, n, z};
  for(int i=0; i<3; ++i) {
    VkImage        image  = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if(!create(c, f[i], nw, nh, image, memory)) {
      Log::e("multiview: cannot create a two-layer target");
      release();
      return false;
      }
    img[i].image  = uint64_t(image);
    img[i].memory = uint64_t(memory);
    }
  diffuse = vulkanBorrowLayeredAttachment(device, VkImage(img[0].image), nw, nh, d, 2);
  normal  = vulkanBorrowLayeredAttachment(device, VkImage(img[1].image), nw, nh, n, 2);
  depth   = vulkanBorrowLayeredDepth     (device, VkImage(img[2].image), nw, nh, z, 2);
  w = nw;
  h = nh;
  frm[0] = d; frm[1] = n; frm[2] = z;
  return true;
  }

#else

LayeredTargets::~LayeredTargets() = default;
void LayeredTargets::release() {}
bool LayeredTargets::resize(Device&, uint32_t, uint32_t, TextureFormat, TextureFormat, TextureFormat) { return false; }

#endif
