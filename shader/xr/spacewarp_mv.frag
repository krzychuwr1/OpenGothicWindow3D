#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

// Application SpaceWarp inputs for one eye of the XR window, at motion-vector resolution:
// - colour: NDC motion of each scene point since the previous frame caused by the game
//   (camera and world), current minus previous; head motion is left to the runtime
// - depth: the scene depth, for the runtime's positional reprojection

#include "scene.glsl"

layout(push_constant, std430) uniform Push {
  mat4 prevViewProject;  // previous game camera seen through the current eye, world space
  vec4 originLwc;
  vec2 invSize;          // 1/size of the motion-vector target
  } push;

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding = 1) uniform sampler2D depth;

layout(location = 0) out vec4 outMotion;

void main() {
  const vec2  uv  = gl_FragCoord.xy*push.invSize;
  const ivec2 at  = ivec2(uv*vec2(textureSize(depth,0)));
  const float z   = texelFetch(depth, at, 0).x;
  const vec2  ndc = uv*2.0-1.0;

  vec4 pos = scene.viewProjectLwcInv*vec4(ndc, z, 1.0);
  pos.xyz /= pos.w;
  const vec3 world = pos.xyz + push.originLwc.xyz;

  vec4 prev = push.prevViewProject*vec4(world, 1.0);
  const vec3 prevNdc = prev.xyz/max(prev.w, 1e-6);

  outMotion    = vec4(vec3(ndc, z) - prevNdc, 0.0);
  gl_FragDepth = z;
  }
