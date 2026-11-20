#version 450

layout(push_constant, std430) uniform Push {
  vec2 size, metresPerPixel;
  float radius, feather;
  uint guard;
  } push;

void main() {
  // Four disjoint strips. Keep the centre out of the coverage shader entirely.
  vec2 band = min(vec2(0.5), (push.radius + push.feather + 2.0*push.metresPerPixel)/push.size);
  vec2 lo = vec2(0), hi = vec2(1);
  if(gl_InstanceIndex==0)
    hi.y = band.y;
  else if(gl_InstanceIndex==1)
    lo.y = 1.0-band.y;
  else {
    lo.y = band.y;
    hi.y = 1.0-band.y;
    if(gl_InstanceIndex==2)
      hi.x = band.x;
    else
      lo.x = 1.0-band.x;
    }
  const vec2 corners[6] = vec2[](vec2(0,0),vec2(1,0),vec2(1,1),
                                 vec2(0,0),vec2(1,1),vec2(0,1));
  gl_Position = vec4(2.0*mix(lo,hi,corners[gl_VertexIndex])-1.0,0,1);
  }
