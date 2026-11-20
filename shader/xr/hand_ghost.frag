#version 450

layout(location = 0) in vec2 inA;
layout(location = 1) in vec2 inB;
layout(location = 2) in vec2 inRadius; // radius in pixels, opacity
layout(location = 3) in vec3 inTint;

layout(location = 0) out vec4 result;

void main() {
  const vec2 p  = gl_FragCoord.xy;
  const vec2 ab = inB-inA;
  const float t = clamp(dot(p-inA,ab)/max(dot(ab,ab),1e-6), 0.0, 1.0);
  const float d = length(p-(inA+ab*t));
  const float r = inRadius.x;
  // A soft translucent body with a brighter rim, like a ghost of the hand
  const float body = 1.0-smoothstep(r-0.75, r+0.75, d);
  const float rim  = smoothstep(r*0.35, r, d);
  const float a    = inRadius.y*body*(0.45+0.55*rim);
  if(a<=0.002)
    discard;
  result = vec4(inTint*a, a); // premultiplied
  }
