#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(binding = 0) uniform sampler2D textureD;

layout(location = 0) in  vec2 UV;
layout(location = 1) in  vec3 inNormal;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 t = texture(textureD, UV);
  if(t.a < 0.5)
    discard;

  float nl = length(inNormal);
  vec3 N = nl > 1e-5 ? (gl_FrontFacing ? inNormal : -inNormal) / nl : vec3(0.0, 1.0, 0.0);

  // Soft studio-style lighting in XR local space so 3D inventory models read clearly over passthrough
  float key  = max(dot(N, normalize(vec3(0.45, 0.75, 0.55))), 0.0);
  float fill = max(dot(N, normalize(vec3(-0.60, 0.20, 0.45))), 0.0);
  float hemi = 0.5 + 0.5 * N.y;

  vec3 light = vec3(0.28, 0.30, 0.34) * hemi
             + vec3(0.18)
             + vec3(0.72, 0.70, 0.65) * key
             + vec3(0.20, 0.24, 0.30) * fill;

  outColor = vec4(t.rgb * light, 1.0);
  }
