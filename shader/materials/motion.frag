#version 450
#extension GL_ARB_separate_shader_objects : enable

// Application SpaceWarp: NDC motion of a moving object's surface since the previous frame
// (current minus previous), written over the camera-only vectors of drawSpaceWarp.

#if defined(ATEST) || defined(PARTICLE_MOTION)
#extension GL_GOOGLE_include_directive : enable
#include "materials_common.glsl"
layout(location = 0) in flat uint bucketId;
layout(location = 1) in Varyings shInp;
layout(push_constant, std430) uniform Push {
#if defined(PARTICLE_MOTION)
  layout(offset=64) vec2 motionSize;
  uint coverage;
#else
  layout(offset=12) uint blendCoverage;
  vec2 motionSize;
#endif
  } push;
#define DIFFUSE_GRAD_SCALE (push.motionSize*scene.screenResInv)
#include "diffuse.glsl"
#endif

layout(location = 4) in vec4 motionCur;
layout(location = 5) in vec4 motionPrev;

layout(location = 0) out vec4 outMotion;

void main() {
#if defined(ATEST)
  vec4 tex = diffuseTex();
  float coverage = tex.a;
  if(push.blendCoverage==2u)
    coverage *= max(tex.r,max(tex.g,tex.b));
  if(coverage<0.5)
    discard;
#endif
#if defined(PARTICLE_MOTION)
  vec4 tex = diffuseTex();
  // The compositor accepts one depth and one vector. Let opaque or bright
  // cores own the sample and keep the background through faint smoke and glow.
  if(push.coverage==1u && tex.a<0.5)
    discard;
  tex *= shInp.color;
  if(push.coverage==2u && tex.a<0.5)
    discard;
  if(push.coverage==3u && max(tex.r,max(tex.g,tex.b))*tex.a<0.5)
    discard;
#endif
  const vec3 cur  = motionCur.xyz /motionCur.w;
  outMotion = motionPrev.w>1e-6 ? vec4(cur - motionPrev.xyz/motionPrev.w, 0.0) : vec4(0);
  // Meta's runtime reads vectors with +y up (OpenGL NDC) also on Vulkan, where NDC +y is down
  outMotion.y = -outMotion.y;
  }
