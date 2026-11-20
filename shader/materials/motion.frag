#version 450
#extension GL_ARB_separate_shader_objects : enable

// Application SpaceWarp: NDC motion of a moving object's surface since the previous frame
// (current minus previous), written over the camera-only vectors of drawSpaceWarp.

#if defined(ATEST)
#extension GL_GOOGLE_include_directive : enable
#include "materials_common.glsl"
layout(location = 0) in flat uint bucketId;
layout(location = 1) in Varyings shInp;
#include "diffuse.glsl"
#endif

layout(location = 4) in vec4 motionCur;
layout(location = 5) in vec4 motionPrev;

layout(location = 0) out vec4 outMotion;

void main() {
#if defined(ATEST)
  if(diffuseTex().a<0.5)
    discard;
#endif
  const vec3 cur  = motionCur.xyz /motionCur.w;
  outMotion = motionPrev.w>1e-6 ? vec4(cur - motionPrev.xyz/motionPrev.w, 0.0) : vec4(0);
  }
