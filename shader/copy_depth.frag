#version 450
#extension GL_ARB_separate_shader_objects : enable

// Copies a depth image into the bound depth attachment (cached shadow casters)
layout(binding = 0) uniform sampler2D src;

void main() {
  gl_FragDepth = texelFetch(src, ivec2(gl_FragCoord.xy), 0).r;
  }
