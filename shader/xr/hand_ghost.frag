#version 450

layout(location = 0) in vec2 inA;
layout(location = 1) in vec2 inB;
layout(location = 2) in vec2 inRadius; // radius in pixels, opacity
layout(location = 3) in vec3 inTint;
layout(location = 4) flat in int inPass;

layout(location = 0) out vec4 result;

void main() {
  const vec2 p  = gl_FragCoord.xy;
  const vec2 ab = inB-inA;
  const float t = clamp(dot(p-inA,ab)/max(dot(ab,ab),1e-6), 0.0, 1.0);
  const float d = length(p-(inA+ab*t));
  const float r = inRadius.x;
  const float cover = 1.0-smoothstep(r-0.75, r+0.75, d);
  if(cover<=0.002)
    discard;
  if(inPass==0) {
    // Pale outline (as VRON Engine's ghost hands): only its rim survives the fill
    const float a = 0.9*inRadius.y*cover;
    result = vec4(vec3(a), a);
    }
  else if(inPass==1) {
    // Dark, mostly opaque fill: written over the outline, not blended with it
    if(cover<0.5)
      discard;
    const float a = 0.75*inRadius.y;
    result = vec4(vec3(0.03,0.03,0.035)*a, a);
    }
  else {
    // Pointer rays and their dots: soft light lines
    const float rim = smoothstep(r*0.35, r, d);
    const float a   = inRadius.y*cover*(0.45+0.55*rim);
    result = vec4(inTint*a, a);
    }
  }
