#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

// Application SpaceWarp inputs for one eye of the XR window, at motion-vector resolution:
// - colour: NDC motion of each scene point since the previous frame caused by the game
//   and tracked eye, current minus previous
// - depth: the scene depth, for the runtime's positional reprojection

#include "scene.glsl"

layout(push_constant, std430) uniform Push {
  mat4 prevViewProject;  // previous view-projection for this eye, world space
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
  // The cleared depth belongs to the sky. Its direction follows camera rotation
  // and the off-axis projection, while translating the eye leaves it unchanged.
  vec4 prev = vec4(0.0);
  bool valid = false;
  if(z==1.0) {
    const float d2 = dot(pos.xyz, pos.xyz);
    if(d2>1e-12) {
      prev  = push.prevViewProject*vec4(pos.xyz*inversesqrt(d2), 0.0);
      valid = prev.w>0.1;
      }
    } else if(abs(pos.w)>1e-6) {
    const vec3 world = pos.xyz/pos.w + push.originLwc.xyz;
    prev  = push.prevViewProject*vec4(world, 1.0);
    // Previous view-space depth is in cm; require safely in front of the previous eye plane
    // so perspective division cannot explode into RGBA16F infinities and tear SpaceWarp.
    valid = prev.w>1.0 && prev.z>=-0.5*prev.w;
    }
  vec3 mv = vec3(0.0);
  if(valid) {
    mv = vec3(ndc - prev.xy/prev.w, z==1.0 ? 0.0 : clamp(z - prev.z/prev.w, -1.0, 1.0));
    if(any(isnan(mv)) || any(isinf(mv)) || any(greaterThan(abs(mv.xy), vec2(1.0))))
      mv = vec3(0.0);
    }
  // Meta's runtime reads vectors with +y up (OpenGL NDC) also on Vulkan, where NDC +y is down
  outMotion = vec4(mv.x, -mv.y, mv.z, 0.0);
  gl_FragDepth = z;
  }
