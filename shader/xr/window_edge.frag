#version 450

layout(location=0) out vec4 result;
#ifdef OPAQUE
void main() {
  result = vec4(0,0,0,1);
  }
#else
layout(push_constant, std430) uniform Push {
  vec2 size, metresPerPixel;
  float radius, feather;
  } push;

void main() {
  vec2 halfSize = 0.5*push.size;
  vec2 q = abs(gl_FragCoord.xy*push.metresPerPixel-halfSize) - halfSize + push.radius;
  float inside = push.radius-length(max(q,0.0))-min(max(q.x,q.y),0.0);
  float aa = 0.5*max(fwidth(inside),1e-7);
  float coverage = push.feather>0.0 ? smoothstep(0.0,max(push.feather,2.0*aa),inside)
                                   : smoothstep(-aa,aa,inside);
#ifdef MOTION
  // The contour belongs to the physical window. Keep the dominant scene
  // samples inside it and pin the exterior samples as for the stationary HUD.
  if(coverage>=0.5)
    discard;
  result = vec4(0);
  gl_FragDepth = 0.0;
#else
  result = vec4(1,1,1,coverage);
#endif
  }
#endif
