#version 450

// Curved window: the eye image is an ordinary perspective view from the eye, framed by the
// rectangle on the window plane that just contains the curved window. Each pixel's ray is traced
// to the window's cylinder (vertical axis, radius R in front of the window centre); the pixel
// belongs to the window when the hit lies inside the window measured along the arc.

layout(location=0) out vec4 result;

layout(push_constant, std430) uniform Push {
  vec4 rect;     // this eye's frame on the window plane: left, right, bottom, top (metres)
  vec4 eye;      // eye position in window space (metres, +z towards the viewer), radius
  vec4 shape;    // half width (arc length), half height, corner radius, feather
  vec2 size;     // image size in pixels
  uint guard;
  } push;

// Signed distance inside the window, in metres along the arc; negative outside.
float insideAt(vec2 pixel) {
  vec2 uv = pixel/push.size;   // image y points down
  vec3 P  = vec3(mix(push.rect.x,push.rect.y,uv.x), mix(push.rect.w,push.rect.z,uv.y), 0.0);
  vec3 E  = push.eye.xyz;
  float R = push.eye.w;
  vec3 d  = P-E;
  float a = d.x*d.x+d.z*d.z;
  float b = 2.0*(E.x*d.x-(R-E.z)*d.z);
  float c = E.x*E.x+(R-E.z)*(R-E.z)-R*R;
  float disc = b*b-4.0*a*c;
  if(a<1e-8 || disc<0.0)
    return -1.0;
  float sq = sqrt(disc);
  // The arc faces the viewer: take the far crossing that lies on the window's side of the axis
  float t = (-b+sq)/(2.0*a);
  vec3  X = E+d*t;
  if(R-X.z<=0.0) {
    t = (-b-sq)/(2.0*a);
    X = E+d*t;
    }
  if(t<=0.0 || R-X.z<=0.0)
    return -1.0;
  vec2 q = vec2(R*atan(X.x,R-X.z), X.y);
  vec2 k = abs(q)-push.shape.xy+push.shape.z;
  return push.shape.z-length(max(k,vec2(0.0)))-min(max(k.x,k.y),0.0);
  }

void main() {
  float inside = insideAt(gl_FragCoord.xy);
  float ramp   = max(push.shape.w, max(fwidth(inside),1e-6));
#ifdef MOTION
  // Outside the window: no scene motion, depth at the window
  if(inside>=0.5*ramp)
    discard;
  result = vec4(0);
  gl_FragDepth = 0.0;
#else
  float coverage = push.shape.w>0.0 ? smoothstep(0.0,ramp,inside)
                                    : smoothstep(-0.5*ramp,0.5*ramp,inside);
  result = vec4(1,1,1,coverage);
#endif
  }
