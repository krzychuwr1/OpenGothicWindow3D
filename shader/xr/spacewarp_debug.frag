#version 450
#extension GL_ARB_separate_shader_objects : enable

// -show motion: the eye's SpaceWarp motion vectors as colour over the dimmed picture. Motion to the
// right is red, up green, left and down blue, stronger for faster motion; still parts stay visible
// but darkened.

layout(push_constant, std430) uniform Push {
  vec2 invSize; // 1/size of the eye image
  } push;

layout(binding = 0) uniform sampler2D motion;

layout(location = 0) out vec4 outColor;

void main() {
  const vec2  mv = textureLod(motion, gl_FragCoord.xy*push.invSize, 0).xy;
  const vec2  k  = mv*vec2(60.0, -60.0);
  const vec3  c  = clamp(vec3(k.x, k.y, -(k.x+k.y)), 0.0, 1.0);
  const float m  = clamp(max(c.r, max(c.g, c.b))*4.0, 0.0, 1.0);
  outColor = vec4(c*m, mix(0.6, 0.95, m));
  }
