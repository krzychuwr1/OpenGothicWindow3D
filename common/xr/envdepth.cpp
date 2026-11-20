#include "envdepth.h"

#if defined(__ANDROID__) && defined(OPENGOTHIC_XR)
#include <Tempest/VulkanInterop>
#include <Tempest/Log>
#include "commandline.h"
#include "shader.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <type_traits>
#include <utility>
#include <mutex>
#include <vector>

namespace {
constexpr int gridW = 64, gridH = 48;
constexpr int columns = 12, rows = 10;
using Point = XrVector3f;

Point operator+(Point a,Point b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Point operator-(Point a,Point b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Point operator*(Point a,float b) { return {a.x*b,a.y*b,a.z*b}; }
float dot(Point a,Point b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
Point cross(Point a,Point b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Point rotate(XrQuaternionf q,Point v) {
  const Point u = {q.x,q.y,q.z}, t = cross(u,v)*2.f;
  return v+t*q.w+cross(u,t);
  }
XrQuaternionf compose(XrQuaternionf a,XrQuaternionf b) {
  const Point p = {a.x,a.y,a.z}, q = {b.x,b.y,b.z};
  const Point v = q*a.w+p*b.w+cross(p,q);
  return {v.x,v.y,v.z,a.w*b.w-dot(p,q)};
  }

struct Sample {
  Point p;   // along the bottom edge, up, forwards towards the viewer, in metres
  int   col,row;
  };

struct Footprint { int l=0,r=0,t=0,b=0; };

struct FitCounts {
  size_t valid=0, footprint=0, hypotheses=0, inliers=0;
  Point low={}, high={};
  };

Footprint supportedFootprint(const std::vector<Sample>& samples,float a,float b,float c,float cellWidth,float cellDepth) {
  std::array<std::bitset<columns>,rows> seen = {};
  std::array<std::bitset<columns>,rows> raised = {};
  for(auto& s:samples) {
    const float residual=s.p.y-(a*s.p.x+b*s.p.z+c);
    if(std::abs(residual)<0.025f)
      seen[size_t(s.row)].set(size_t(s.col));
    else if(residual>0.065f)
      raised[size_t(s.row)].set(size_t(s.col));
    }
  // Bridge a single missing sample cell only where measured support brackets it.
  auto covered = seen;
  for(int y=0;y<rows;++y) {
    auto& row = covered[size_t(y)];
    row |= (seen[size_t(y)]<<1)&(seen[size_t(y)]>>1);
    if(y>0 && y+1<rows)
      row |= seen[size_t(y-1)]&seen[size_t(y+1)];
    // Leave a foreground object visibly unlit even if there is supporting
    // tabletop around it. Never interpolate across a measured occluder.
    row &= ~raised[size_t(y)];
    }
  Footprint best;
  for(int top=0;top<rows;++top) {
    std::bitset<columns> strip;
    strip.set();
    for(int y=top;y<rows;++y) {
      strip &= covered[size_t(y)];
      if(strip.none())
        break;
      // A desk ending to one side of the window still catches its light.
      for(int r=0;r<columns;) {
        while(r<columns && !strip.test(size_t(r))) ++r;
        const int l=r;
        while(r<columns && strip.test(size_t(r))) ++r;
        if(float(r-l)*cellWidth>=0.27f && float(y+1-top)*cellDepth>=0.19f &&
           (r-l)*(y+1-top)>(best.r-best.l)*(best.b-best.t))
          best = {l,r,top,y+1};
        }
      }
    }
  return best;
  }

std::optional<EnvironmentDepth::Surface> fitSurface(const float* samples, uint32_t imageW, uint32_t imageH,
                                                     const XrEnvironmentDepthImageMETA& depth,
                                                     const XrPosef& window,XrExtent2Df size,FitCounts* counts=nullptr) {
  // The depth texture is in Vulkan row order. The first row is the upper FOV tangent.
  const auto& view = depth.views[0];
  const float L=std::tan(view.fov.angleLeft),R=std::tan(view.fov.angleRight);
  const float T=std::tan(view.fov.angleUp),B=std::tan(view.fov.angleDown);
  if(!std::isfinite(L) || !std::isfinite(R) || !std::isfinite(T) || !std::isfinite(B) ||
     !(depth.nearZ>0 && L<R && B<T) || !std::isfinite(depth.nearZ) || imageW==0 || imageH==0)
    return {};
  const float invFar = std::isfinite(depth.farZ) && depth.farZ>depth.nearZ ? 1.f/depth.farZ : 0.f;
  Point right = rotate(window.orientation,{1,0,0});
  right.y = 0;
  if(dot(right,right)<0.001f)
    return {};
  right = right*(1.f/std::sqrt(dot(right,right)));
  const Point up = {0,1,0}, front = cross(right,up);
  const Point foot = window.position+rotate(window.orientation,{0,-0.5f*size.height,0});
  // The portal is free-standing and can extend into a real desk. Accept the
  // portion of that desk below eye level even if it intersects the lower screen.
  const float maxHeight = std::min(view.pose.position.y-foot.y-0.025f,0.5f*size.height+0.15f);
  const float halfW = std::clamp(0.46f*size.width,0.22f,1.2f);
  const float reach = std::clamp(0.35f+0.28f*size.width,0.55f,1.35f);
  std::vector<Sample> cloud;
  cloud.reserve(gridW*gridH);
  for(int y=0;y<gridH;++y) {
    for(int x=0;x<gridW;++x) {
      const size_t i = size_t(y*gridW+x);
      const float d = samples[i];
      if(!(d>0 && d<1))
        continue;
      const float distance = 1.f/((1.f-d)/depth.nearZ+d*invFar);
      if(!(distance>=0.12f && distance<=5.5f))
        continue;
      const float u = (float(uint32_t(2*x+1)*imageW/uint32_t(2*gridW))+0.5f)/float(imageW);
      const float v = (float(uint32_t(2*y+1)*imageH/uint32_t(2*gridH))+0.5f)/float(imageH);
      const Point ray = {L+u*(R-L),T+v*(B-T),-1};
      const Point world = view.pose.position+rotate(view.pose.orientation,ray*distance)-foot;
      if(!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z))
        continue;
      if(counts) ++counts->valid;
      const Point p = {dot(world,right),world.y,dot(world,front)};
      if(std::abs(p.x)>=halfW || p.z<0 || p.z>=reach || p.y>maxHeight || p.y< -2.3f)
        continue;
      const int col = std::min(columns-1,int((p.x+halfW)*float(columns)/(2.f*halfW)));
      const int row = std::min(rows-1,int(p.z*float(rows)/reach));
      cloud.push_back({p,col,row});
      if(counts) {
        if(cloud.size()==1) {
          counts->low=counts->high=p;
          } else {
          counts->low={std::min(counts->low.x,p.x),std::min(counts->low.y,p.y),std::min(counts->low.z,p.z)};
          counts->high={std::max(counts->high.x,p.x),std::max(counts->high.y,p.y),std::max(counts->high.z,p.z)};
          }
        }
      }
    }
  if(counts) counts->footprint=cloud.size();
  if(cloud.size()<12)
    return {};

  float bestScore = 0, a=0,b=0,c=0;
  Footprint bounds;
  auto consider = [&](float ax,float bz,float h) {
    if(h>maxHeight || h< -2.3f)
      return;
    if(counts) ++counts->hypotheses;
    auto block = supportedFootprint(cloud,ax,bz,h,2.f*halfW/float(columns),reach/float(rows));
    const float w = 2.f*halfW*float(block.r-block.l)/float(columns);
    const float d = reach*float(block.b-block.t)/float(rows);
    if(w<0.27f || d<0.19f)
      return;
    // A tabletop touching the window intercepts its light before the larger
    // floor visible in front of the table. Weight measured area by distance
    // from the screen edge, with a finite radius for a surface in contact.
    const float drop = 0.25f+std::max(0.f,-h);
    const float gap  = reach*float(block.t)/float(rows);
    const float score = w*d/(drop*drop+gap*gap);
    if(score>bestScore) {
      a=ax; b=bz; c=h; bounds=block; bestScore=score;
      }
    };
  // At grazing angles adjacent rays can miss or land on opposite surfaces.
  // Test height slices and well-separated sample triples across the footprint.
  // A fixed seed gives identical results for identical depth maps.
  const size_t stride=(cloud.size()+63)/64;
  for(size_t i=0;i<cloud.size();i+=stride)
    consider(0,0,cloud[i].p.y);
  uint32_t seed=1698227261u;
  auto pick = [&]() {
    seed=1664525u*seed+1013904223u;
    return cloud[(uint64_t(seed)*cloud.size())>>32].p;
    };
  for(int trial=0;trial<192;++trial) {
    const Point p=pick(), q=pick(), r=pick();
    const auto n=cross(q-p,r-p);
    if(std::abs(n.y)<0.012f || n.x*n.x+n.z*n.z>0.09f*n.y*n.y)
      continue;
    const float ax=-n.x/n.y, bz=-n.z/n.y;
    consider(ax,bz,p.y-ax*p.x-bz*p.z);
    }
  if(bestScore==0)
    return {};

  // Fit the selected tabletop or floor to all agreeing points inside its measured footprint.
  double sx=0,sz=0,sy=0,sxx=0,szz=0,sxz=0,sxy=0,szy=0,count=0;
  for(auto& s:cloud) {
    const Point p = s.p;
    if(s.col<bounds.l || s.col>=bounds.r || s.row<bounds.t || s.row>=bounds.b ||
       std::abs(p.y-(a*p.x+b*p.z+c))>=0.025f)
      continue;
    sx+=p.x; sz+=p.z; sy+=p.y;
    sxx+=double(p.x)*p.x; szz+=double(p.z)*p.z; sxz+=double(p.x)*p.z;
    sxy+=double(p.x)*p.y; szy+=double(p.z)*p.y;
    ++count;
    }
  if(counts) counts->inliers=size_t(count);
  if(count<12)
    return {};
  sxx-=sx*sx/count; szz-=sz*sz/count; sxz-=sx*sz/count;
  sxy-=sx*sy/count; szy-=sz*sy/count;
  const double det = sxx*szz-sxz*sxz;
  // A strip of points on a wall or desk edge can span multiple grid cells.
  // Demand at least 3.5 cm RMS support in the least-spread horizontal axis.
  const double minVariance=(sxx+szz-std::hypot(sxx-szz,2*sxz))/(2*count);
  if(det<=1e-8 || minVariance<0.035*0.035)
    return {};
  a=float((szz*sxy-sxz*szy)/det);
  b=float((sxx*szy-sxz*sxy)/det);
  c=float((sy-double(a)*sx-double(b)*sz)/count);
  if(a*a+b*b>0.09f || c>maxHeight || c< -2.3f)
    return {};

  const float x = halfW*(float(bounds.l+bounds.r)/float(columns)-1.f);
  const float z = reach*0.5f*float(bounds.t+bounds.b)/float(rows);
  auto normal = (up-right*a-front*b)*(1.f/std::sqrt(1.f+a*a+b*b));
  const float sn = std::sqrt(2.f+2.f*normal.y);
  const XrQuaternionf tilt = {normal.z/sn,0,-normal.x/sn,sn*0.5f};
  const float yaw = 0.5f*std::atan2(front.x,front.z);
  const XrQuaternionf facing = {0,std::sin(yaw),0,std::cos(yaw)};
  constexpr float k = 0.70710678f;
  EnvironmentDepth::Surface out;
  out.pose.orientation = compose(tilt,compose(facing,{-k,0,0,k}));
  out.pose.position = foot+right*x+front*z+up*(a*x+b*z+c)+normal*0.004f;
  out.uvOffset = {float(bounds.l)/float(columns),float(bounds.t)/float(rows)};
  out.uvScale = {float(bounds.r-bounds.l)/float(columns),float(bounds.b-bounds.t)/float(rows)};
  out.size = {2.f*halfW*out.uvScale.x*std::sqrt(1.f+a*a),reach*out.uvScale.y*std::sqrt(1.f+b*b)};
  out.drop = std::max(0.f,-c);
  return out;
  }
} // namespace

namespace {
bool vkOk(VkResult result,const char* operation) {
  if(result==VK_SUCCESS) return true;
  Tempest::Log::e("XR surface depth ",operation," failed (",int(result),")");
  return false;
  }
bool xrOk(XrResult result,const char* operation) {
  if(result==XR_SUCCESS) return true;
  Tempest::Log::e("XR surface depth ",operation," failed (",int(result),")");
  return false;
  }
}

struct EnvironmentDepth::Impl {
  explicit Impl(Tempest::Device& device) : vk(Tempest::vulkanNativeContext(device)) {}
  ~Impl();
  bool initialize(XrInstance instance,XrSystemId system,XrSession session);
  bool createGpu();
  bool sample(uint32_t index);
  bool stop();

  Tempest::VulkanNativeContext vk;
  XrEnvironmentDepthProviderMETA provider = XR_NULL_HANDLE;
  XrEnvironmentDepthSwapchainMETA chain = XR_NULL_HANDLE;
  PFN_xrDestroyEnvironmentDepthProviderMETA destroyProvider = nullptr;
  PFN_xrStartEnvironmentDepthProviderMETA startProvider = nullptr;
  PFN_xrStopEnvironmentDepthProviderMETA stopProvider = nullptr;
  PFN_xrDestroyEnvironmentDepthSwapchainMETA destroyChain = nullptr;
  PFN_xrAcquireEnvironmentDepthImageMETA acquire = nullptr;
  std::vector<VkImageView> views;
  uint32_t imageW = 0, imageH = 0;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  bool coherent = false;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptors = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool wanted = false, running = false, pending = false, acceptResult = false;
  bool trace = CommandLine::inst().debugShow("surfacelight");
  unsigned maps = 0;
  bool waitingForStart = false;
  XrPosef window;
  XrExtent2Df size;
  XrTime startedAt = 0, nextMap = 0;
  XrEnvironmentDepthImageMETA frame = {XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
  };

bool EnvironmentDepth::Impl::initialize(XrInstance instance,XrSystemId system,XrSession session) {
  if(!vk.queueLock) return false;
  XrSystemEnvironmentDepthPropertiesMETA caps = {XR_TYPE_SYSTEM_ENVIRONMENT_DEPTH_PROPERTIES_META};
  XrSystemProperties properties = {XR_TYPE_SYSTEM_PROPERTIES,&caps};
  if(!xrOk(xrGetSystemProperties(instance,system,&properties),"system properties") || !caps.supportsEnvironmentDepth)
    return false;

  PFN_xrCreateEnvironmentDepthProviderMETA createProvider = nullptr;
  PFN_xrCreateEnvironmentDepthSwapchainMETA createChain = nullptr;
  PFN_xrGetEnvironmentDepthSwapchainStateMETA getState = nullptr;
  PFN_xrEnumerateEnvironmentDepthSwapchainImagesMETA enumerate = nullptr;
  auto load = [instance](const char* name,auto& fn) {
    PFN_xrVoidFunction f=nullptr;
    if(!xrOk(xrGetInstanceProcAddr(instance,name,&f),name) || !f) return false;
    fn=reinterpret_cast<std::remove_reference_t<decltype(fn)>>(f);
    return true;
    };
  if(!load("xrCreateEnvironmentDepthProviderMETA",createProvider) ||
     !load("xrDestroyEnvironmentDepthProviderMETA",destroyProvider) ||
     !load("xrStartEnvironmentDepthProviderMETA",startProvider) ||
     !load("xrStopEnvironmentDepthProviderMETA",stopProvider) ||
     !load("xrCreateEnvironmentDepthSwapchainMETA",createChain) ||
     !load("xrDestroyEnvironmentDepthSwapchainMETA",destroyChain) ||
     !load("xrGetEnvironmentDepthSwapchainStateMETA",getState) ||
     !load("xrEnumerateEnvironmentDepthSwapchainImagesMETA",enumerate) ||
     !load("xrAcquireEnvironmentDepthImageMETA",acquire)) return false;
  XrEnvironmentDepthProviderCreateInfoMETA pi = {XR_TYPE_ENVIRONMENT_DEPTH_PROVIDER_CREATE_INFO_META};
  XrEnvironmentDepthSwapchainCreateInfoMETA ci = {XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_CREATE_INFO_META};
  XrEnvironmentDepthProviderMETA newProvider = XR_NULL_HANDLE;
  if(!xrOk(createProvider(session,&pi,&newProvider),"create provider") || !newProvider) return false;
  provider = newProvider;
  if(caps.supportsHandRemoval) {
    PFN_xrSetEnvironmentDepthHandRemovalMETA setHands = nullptr;
    XrEnvironmentDepthHandRemovalSetInfoMETA hands = {XR_TYPE_ENVIRONMENT_DEPTH_HAND_REMOVAL_SET_INFO_META,nullptr,XR_TRUE};
    if(load("xrSetEnvironmentDepthHandRemovalMETA",setHands))
      xrOk(setHands(provider,&hands),"remove hands");
    }
  XrEnvironmentDepthSwapchainMETA newChain = XR_NULL_HANDLE;
  if(!xrOk(createChain(provider,&ci,&newChain),"create swapchain") || !newChain) return false;
  chain = newChain;
  XrEnvironmentDepthSwapchainStateMETA state = {XR_TYPE_ENVIRONMENT_DEPTH_SWAPCHAIN_STATE_META};
  if(!xrOk(getState(chain,&state),"swapchain dimensions") || !state.width || !state.height) return false;
  imageW=state.width; imageH=state.height;
  uint32_t count=0;
  if(!xrOk(enumerate(chain,0,&count,nullptr),"image count") || !count) return false;
  std::vector<XrSwapchainImageVulkanKHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  if(!xrOk(enumerate(chain,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"enumerate images") ||
     count==0 || count>images.size()) return false;
  views.resize(count);
  if(trace) Tempest::Log::i("XR surface depth swapchain ",imageW,"x",imageH,", images=",count);
  for(uint32_t i=0;i<count;++i) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=images[i].image;
    vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
    vi.format=VK_FORMAT_D16_UNORM;
    vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}; // left eye
    VkImageView view = VK_NULL_HANDLE;
    if(!vkOk(vkCreateImageView(vk.device,&vi,nullptr,&view),"depth image view")) return false;
    views[i]=view;
    }
  return createGpu();
  }

bool EnvironmentDepth::Impl::createGpu() {
  const auto dev=vk.device;
  VkBufferCreateInfo bi={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size=gridW*gridH*sizeof(float);
  bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if(!vkOk(vkCreateBuffer(dev,&bi,nullptr,&buffer),"create sample buffer")) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev,buffer,&req);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(vk.physicalDevice,&mp);
  uint32_t type=mp.memoryTypeCount;
  for(uint32_t i=0;i<mp.memoryTypeCount;++i) {
    const auto flags=mp.memoryTypes[i].propertyFlags;
    if((req.memoryTypeBits&(uint32_t(1)<<i))==0 || (flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)==0) continue;
    type=i;
    coherent=(flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    if(coherent) break;
    }
  if(type==mp.memoryTypeCount) return false;
  VkMemoryAllocateInfo ma={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ma.allocationSize=req.size;
  ma.memoryTypeIndex=type;
  if(!vkOk(vkAllocateMemory(dev,&ma,nullptr,&memory),"allocate sample buffer") ||
     !vkOk(vkBindBufferMemory(dev,buffer,memory,0),"bind sample buffer") ||
     !vkOk(vkMapMemory(dev,memory,0,VK_WHOLE_SIZE,0,&mapped),"map sample buffer")) return false;
  VkSamplerCreateInfo si={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter=si.minFilter=VK_FILTER_NEAREST;
  si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if(!vkOk(vkCreateSampler(dev,&si,nullptr,&sampler),"create sampler")) return false;
  VkDescriptorSetLayoutBinding bindings[2]={
    {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,&sampler},
    {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo li={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount=2; li.pBindings=bindings;
  if(!vkOk(vkCreateDescriptorSetLayout(dev,&li,nullptr,&setLayout),"descriptor layout")) return false;
  VkPipelineLayoutCreateInfo pi={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pi.setLayoutCount=1; pi.pSetLayouts=&setLayout;
  if(!vkOk(vkCreatePipelineLayout(dev,&pi,nullptr,&pipelineLayout),"pipeline layout")) return false;
  const auto code=GothicShader::get("environment_depth.comp.sprv");
  if(code.len==0 || code.len%sizeof(uint32_t)!=0) return false;
  std::vector<uint32_t> words(code.len/sizeof(uint32_t));
  std::memcpy(words.data(),code.data,code.len);
  VkShaderModuleCreateInfo sm={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  sm.pCode=words.data(); sm.codeSize=code.len;
  VkShaderModule shader=VK_NULL_HANDLE;
  if(!vkOk(vkCreateShaderModule(dev,&sm,nullptr,&shader),"sampling shader")) return false;
  VkComputePipelineCreateInfo cp={VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cp.layout=pipelineLayout;
  cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
  cp.stage.module=shader; cp.stage.pName="main";
  const auto result=vkCreateComputePipelines(dev,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline);
  vkDestroyShaderModule(dev,shader,nullptr);
  if(!vkOk(result,"sampling pipeline")) return false;
  VkDescriptorPoolSize pools[2]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
  VkDescriptorPoolCreateInfo dp={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp.maxSets=1; dp.poolSizeCount=2; dp.pPoolSizes=pools;
  if(!vkOk(vkCreateDescriptorPool(dev,&dp,nullptr,&descriptors),"descriptor pool")) return false;
  VkDescriptorSetAllocateInfo da={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  da.descriptorPool=descriptors; da.descriptorSetCount=1; da.pSetLayouts=&setLayout;
  if(!vkOk(vkAllocateDescriptorSets(dev,&da,&set),"allocate descriptor set")) return false;
  VkDescriptorBufferInfo dst={buffer,0,bi.size};
  VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet=set; write.dstBinding=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.descriptorCount=1; write.pBufferInfo=&dst;
  vkUpdateDescriptorSets(dev,1,&write,0,nullptr);
  VkCommandPoolCreateInfo poolInfo={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.queueFamilyIndex=vk.queueFamily;
  if(!vkOk(vkCreateCommandPool(dev,&poolInfo,nullptr,&pool),"command pool")) return false;
  VkCommandBufferAllocateInfo cmdInfo={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cmdInfo.commandPool=pool; cmdInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cmdInfo.commandBufferCount=1;
  if(!vkOk(vkAllocateCommandBuffers(dev,&cmdInfo,&cmd),"command buffer")) return false;
  VkFenceCreateInfo fi={VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return vkOk(vkCreateFence(dev,&fi,nullptr,&fence),"create fence");
  }

bool EnvironmentDepth::Impl::sample(uint32_t index) {
  if(index>=views.size()) return false;
  const auto dev=vk.device;
  if(!vkOk(vkResetCommandPool(dev,pool,0),"reset commands") ||
     !vkOk(vkResetFences(dev,1,&fence),"reset fence")) return false;
  VkDescriptorImageInfo src={sampler,views[index],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet=set; write.dstBinding=0; write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.descriptorCount=1; write.pImageInfo=&src;
  vkUpdateDescriptorSets(dev,1,&write,0,nullptr);
  VkCommandBufferBeginInfo bi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if(!vkOk(vkBeginCommandBuffer(cmd,&bi),"begin sampling")) return false;
  // Use the binding queue and leave the runtime's D16 image in shader-read layout.
  VkMemoryBarrier read={VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  read.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;
  read.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&read,0,nullptr,0,nullptr);
  vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
  vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&set,0,nullptr);
  vkCmdDispatch(cmd,uint32_t(gridW/8),uint32_t(gridH/8),1);
  VkMemoryBarrier host={VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
  if(!vkOk(vkEndCommandBuffer(cmd),"end sampling")) return false;
  VkSubmitInfo submission={VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submission.commandBufferCount=1; submission.pCommandBuffers=&cmd;
  // Caller holds the binding queue lock across acquire and submit.
  const auto result=vkQueueSubmit(vk.queue,1,&submission,fence);
  pending=result==VK_SUCCESS || result==VK_ERROR_DEVICE_LOST;
  return vkOk(result,"submit samples");
  }

bool EnvironmentDepth::Impl::stop() {
  if(!running)
    return true;
  const auto result = stopProvider(provider);
  if(result!=XR_ERROR_UNEXPECTED_STATE_PASSTHROUGH_FB && !xrOk(result,"stop provider"))
    return false;
  running=false;
  if(trace) Tempest::Log::i("XR surface depth provider stopped");
  return true;
  }

EnvironmentDepth::Impl::~Impl() {
  stop();
  const auto dev=vk.device;
  if(pending) {
    auto result=vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX);
    while(result==VK_TIMEOUT || result==VK_NOT_READY || result==VK_ERROR_OUT_OF_HOST_MEMORY ||
          result==VK_ERROR_OUT_OF_DEVICE_MEMORY) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      result=vkGetFenceStatus(dev,fence);
      }
    }
  if(fence) vkDestroyFence(dev,fence,nullptr);
  if(pool) vkDestroyCommandPool(dev,pool,nullptr);
  if(descriptors) vkDestroyDescriptorPool(dev,descriptors,nullptr);
  if(pipeline) vkDestroyPipeline(dev,pipeline,nullptr);
  if(pipelineLayout) vkDestroyPipelineLayout(dev,pipelineLayout,nullptr);
  if(setLayout) vkDestroyDescriptorSetLayout(dev,setLayout,nullptr);
  if(sampler) vkDestroySampler(dev,sampler,nullptr);
  if(mapped) vkUnmapMemory(dev,memory);
  if(buffer) vkDestroyBuffer(dev,buffer,nullptr);
  if(memory) vkFreeMemory(dev,memory,nullptr);
  for(auto view:views)
    if(view) vkDestroyImageView(dev,view,nullptr);
  if(chain) destroyChain(chain);
  if(provider) destroyProvider(provider);
  }

EnvironmentDepth::EnvironmentDepth()=default;
EnvironmentDepth::~EnvironmentDepth()=default;

bool EnvironmentDepth::initialize(XrInstance instance,XrSystemId system,XrSession session,Tempest::Device& device) {
  if(impl) return true;
  try {
    auto candidate=std::make_unique<Impl>(device);
    if(!candidate->initialize(instance,system,session)) return false;
    impl=std::move(candidate);
    return true;
    }
  catch(const std::exception& e) {
    Tempest::Log::e("XR surface depth initialization: ",e.what());
    return false;
    }
  }

void EnvironmentDepth::request(const XrPosef& window,XrExtent2Df size) {
  cancel();
  if(impl && size.width>0 && size.height>0) {
    impl->window=window;
    impl->size=size;
    impl->wanted=true;
    impl->maps=0;
    impl->nextMap=0;
    }
  }

void EnvironmentDepth::update(XrSpace local,XrTime displayTime) {
  if(!impl || (!impl->wanted && !impl->pending)) return;
  auto& s=*impl;
  if(s.pending) {
    // The previous frame has released the acquired image. Only the app-owned
    // sample buffer is read below, after its compute dispatch completes.
    const auto status=vkGetFenceStatus(s.vk.device,s.fence);
    if(status==VK_NOT_READY) return;
    if(status!=VK_SUCCESS) {
      if(status==VK_ERROR_DEVICE_LOST) s.pending=false;
      s.acceptResult=false;
      s.wanted=false;
      vkOk(status,"poll sample fence");
      if(!s.stop()) impl.reset();
      return;
      }
    s.pending=false;
    if(s.acceptResult) {
      VkMappedMemoryRange range={VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory=s.memory; range.size=VK_WHOLE_SIZE;
      if(s.coherent || vkOk(vkInvalidateMappedMemoryRanges(s.vk.device,1,&range),"invalidate samples")) {
        try {
          FitCounts counts;
          fitted=fitSurface(static_cast<const float*>(s.mapped),s.imageW,s.imageH,s.frame,s.window,s.size,
                            s.trace ? &counts : nullptr);
          if(s.trace)
            Tempest::Log::i("XR surface samples valid=",counts.valid," footprint=",counts.footprint,
                            " planeHypotheses=",counts.hypotheses," fitted=",counts.inliers);
          if(s.trace && counts.footprint)
            Tempest::Log::i("XR surface measured bounds relative to bottom edge, right/up/forward: min=",
                            counts.low.x,",",counts.low.y,",",counts.low.z," max=",counts.high.x,",",counts.high.y,",",counts.high.z);
          if(s.trace && fitted) {
            const auto& f=*fitted;
            const auto n=rotate(f.pose.orientation,{0,0,1});
            Tempest::Log::i("XR surface fitted normal LOCAL=",n.x,",",n.y,",",n.z);
            for(int y : {-1,1})
              for(int x : {-1,1}) {
                const auto p=f.pose.position+rotate(f.pose.orientation,{x*f.size.width*0.5f,y*f.size.height*0.5f,0});
                Tempest::Log::i("XR surface fitted corner LOCAL=",p.x,",",p.y,",",p.z);
                }
            }
          } catch(const std::bad_alloc&) { fitted.reset(); }
        }
      if(fitted)
        Tempest::Log::i("XR surface depth found ",fitted->size.width," x ",fitted->size.height," m, height ",
                        fitted->pose.position.y," m LOCAL, drop ",fitted->drop," m");
      else if(s.maps<8 && displayTime-s.startedAt<8000000000LL) {
        // The first map after provider startup can have holes. Give tracking
        // a short bounded acquisition burst, then sleep until the next placement.
        s.wanted=true;
        s.nextMap=displayTime+150000000;
        }
      else {
        Tempest::Log::i("XR surface depth found no supported plane under window");
        failed=true;
        }
      }
    s.acceptResult=false;
    if(!s.wanted) {
      if(!s.stop()) impl.reset();
      return;
      }
    }
  if(!s.running) {
    if(!xrOk(s.startProvider(s.provider),"start provider")) { s.wanted=false; return; }
    s.running=true;
    s.startedAt=displayTime;
    s.waitingForStart=true;
    if(s.trace) Tempest::Log::i("XR surface depth provider started; window centre ",
                                s.window.position.x,",",s.window.position.y,",",s.window.position.z,
                                ", size ",s.size.width,"x",s.size.height);
    // Start takes effect at a frame boundary on Quest. Keep the provider
    // running and acquire the first map in a subsequent render frame.
    return;
    }
  // Starting the depth sensor alone can take two seconds; leave room for several maps
  if(displayTime-s.startedAt>8000000000LL) {
    Tempest::Log::i("XR surface depth probe timed out, maps=",s.maps);
    failed=true;
    cancel();
    return;
    }
  if(displayTime<s.nextMap) return;
  XrEnvironmentDepthImageAcquireInfoMETA info={XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_ACQUIRE_INFO_META,nullptr,local,displayTime};
  s.frame={XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_META};
  for(auto& v:s.frame.views) v={XR_TYPE_ENVIRONMENT_DEPTH_IMAGE_VIEW_META};
  {
    std::lock_guard<std::mutex> guard(*s.vk.queueLock);
    const auto result=s.acquire(s.provider,&info,&s.frame);
    if(result==XR_ERROR_CALL_ORDER_INVALID && s.maps==0) {
      if(s.waitingForStart) Tempest::Log::i("XR surface waiting for started provider to accept its first frame");
      s.waitingForStart=false;
      s.nextMap=displayTime+100000000;
      return; // bounded by the probe timeout
      }
    if(result==XR_ENVIRONMENT_DEPTH_NOT_AVAILABLE_META) return;
    s.wanted=false;
    if(xrOk(result,"acquire map")) {
      ++s.maps;
      if(s.trace) Tempest::Log::i("XR surface depth map ",s.maps," index=",s.frame.swapchainIndex,
                                  " near=",s.frame.nearZ," far=",s.frame.farZ,
                                  " eyeHeight=",s.frame.views[0].pose.position.y);
      if(s.sample(s.frame.swapchainIndex)) {
        s.acceptResult=true;
        return;
        }
      }
  }
  if(!s.stop())
    impl.reset();
  }

void EnvironmentDepth::cancel() {
  fitted.reset();
  if(impl) {
    impl->wanted=impl->acceptResult=false;
    if(!impl->stop())
      impl.reset();
    }
  }

void EnvironmentDepth::reset() {
  cancel();
  impl.reset();
  }
#endif

