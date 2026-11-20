#version 450
#extension GL_ARB_separate_shader_objects : enable

// Application SpaceWarp: NDC motion of a moving object's surface since the previous frame
// (current minus previous), written over the camera-only vectors of drawSpaceWarp.

layout(location = 4) in vec4 motionCur;
layout(location = 5) in vec4 motionPrev;

layout(location = 0) out vec4 outMotion;

void main() {
  const vec3 cur  = motionCur.xyz /motionCur.w;
  const vec3 prev = motionPrev.xyz/motionPrev.w;
  outMotion = vec4(cur - prev, 0.0);
  }
