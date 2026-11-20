#version 450

layout(binding = 0) uniform sampler2D hudTransmittance;
layout(binding = 1) uniform sampler2D itemDepth;
layout(push_constant) uniform Push { uint hasItems; } push;
layout(location = 0) out vec4 outMotion;

void main() {
  if(texelFetch(hudTransmittance, ivec2(gl_FragCoord.xy), 0).r>0.0) {
    if(push.hasItems==0u)
      discard;
    vec2 uv = gl_FragCoord.xy/vec2(textureSize(hudTransmittance,0));
    if(textureLod(itemDepth, uv, 0.0).r>=1.0)
      discard;
    }
  // Opaque shared UI lies on the physical window plane at fixed window coordinates.
  outMotion = vec4(0.0);
  gl_FragDepth = 0.0;
  }
