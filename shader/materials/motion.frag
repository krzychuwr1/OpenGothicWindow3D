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
  vec3 mv = vec3(0.0);
  if(motionCur.w>1e-4 && motionPrev.w>1.0 && motionPrev.z>=-0.5*motionPrev.w) {
    const vec3 cur  = motionCur.xyz/motionCur.w;
    const vec3 prev = motionPrev.xyz/motionPrev.w;
    mv = vec3(cur.xy - prev.xy, clamp(cur.z - prev.z, -1.0, 1.0));
    if(any(isnan(mv)) || any(isinf(mv)) || any(greaterThan(abs(mv.xy), vec2(1.0))))
      mv = vec3(0.0);
    }
  // Meta's runtime reads vectors with +y up (OpenGL NDC) also on Vulkan, where NDC +y is down
  outMotion = vec4(mv.x, -mv.y, mv.z, 0.0);
  }
