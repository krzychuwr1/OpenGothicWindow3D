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
  uint guard;
  } push;

float distanceAt(vec2 pixel) {
  vec2 halfSize = 0.5*push.size;
  vec2 q = abs(pixel*push.metresPerPixel-halfSize) - halfSize + push.radius;
  return push.radius-length(max(q,0.0))-min(max(q.x,q.y),0.0);
  }

float coverageAt(float inside) {
  if(push.feather>0.0) {
    float ramp = push.feather;
    // A wider feather already covers the maximum distance change across a quad.
    // For a subpixel feather, use the same neighbours as the motion contour.
    if(ramp<push.metresPerPixel.x+push.metresPerPixel.y)
      ramp = max(ramp,max(fwidthFine(inside),1e-7));
    return smoothstep(0.0,ramp,inside);
    }
  float aa = 0.5*max(fwidth(inside),1e-7);
  return smoothstep(-aa,aa,inside);
  }

#ifdef MOTION
bool sceneSide(vec2 pixel) {
  float inside = distanceAt(pixel);
  if(push.feather==0.0)
    return inside>=0.0;
  float ramp = push.feather;
  if(ramp<push.metresPerPixel.x+push.metresPerPixel.y) {
    // A subpixel feather uses the coverage shader's derivative width. Evaluate
    // the neighbours in this pixel's own 2x2 quad, including for offset samples.
    vec2 stepToMate = 1.0-2.0*mod(floor(pixel),2.0);
    float dx = distanceAt(pixel+vec2(stepToMate.x,0.0))-inside;
    float dy = distanceAt(pixel+vec2(0.0,stepToMate.y))-inside;
    ramp = max(ramp,max(abs(dx)+abs(dy),1e-7));
    }
  return inside>=0.5*ramp;
  }
#endif

void main() {
  vec2 pixel = gl_FragCoord.xy;
#ifdef MOTION
  // Use the same halfway test for the centre and its four neighbours.
  if(sceneSide(pixel) && (push.guard==0u ||
     (sceneSide(pixel-vec2(1,0)) && sceneSide(pixel+vec2(1,0)) &&
      sceneSide(pixel-vec2(0,1)) && sceneSide(pixel+vec2(0,1)))))
    discard;
  result = vec4(0);
  gl_FragDepth = 0.0;
#else
  result = vec4(1,1,1,coverageAt(distanceAt(pixel)));
#endif
  }
#endif
