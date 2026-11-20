#version 450

// Ghost hands and pointer rays: each instance is a capsule (two points and a radius) in the
// LOCAL space, drawn as a screen-space quad around its projected segment.

struct Capsule {
  vec4 a;    // xyz, radius (metres)
  vec4 b;    // xyz, opacity
  vec4 tint; // rgb, unused
  };

layout(binding = 0, std430) readonly buffer Capsules {
  Capsule caps[];
  };

layout(push_constant, std430) uniform Push {
  mat4 viewProject; // LOCAL -> clip of this eye (Vulkan, y down)
  vec4 size;        // image width, height, focal length in pixels (vertical), unused
  } push;

layout(location = 0) out vec2 outA;
layout(location = 1) out vec2 outB;
layout(location = 2) out vec2 outRadius;  // radius in pixels, opacity
layout(location = 3) out vec3 outTint;

void main() {
  const Capsule c = caps[gl_InstanceIndex];
  vec4 pa = push.viewProject*vec4(c.a.xyz,1.0);
  vec4 pb = push.viewProject*vec4(c.b.xyz,1.0);
  if(pa.w<0.02 || pb.w<0.02) {
    gl_Position = vec4(2.0,2.0,0.0,1.0); // behind the eye: nothing to draw
    return;
    }
  const vec2 sa = (pa.xy/pa.w*0.5+0.5)*push.size.xy;
  const vec2 sb = (pb.xy/pb.w*0.5+0.5)*push.size.xy;
  const float r = max(c.a.w*push.size.z/min(pa.w,pb.w), 1.0) + 1.5;

  vec2 dir = sb-sa;
  float len = length(dir);
  dir = len>1e-3 ? dir/len : vec2(1.0,0.0);
  const vec2 side = vec2(-dir.y,dir.x);
  const vec2 corner[6] = vec2[](vec2(-1,-1),vec2(1,-1),vec2(1,1), vec2(-1,-1),vec2(1,1),vec2(-1,1));
  const vec2 k = corner[gl_VertexIndex];
  const vec2 p = (k.x<0.0 ? sa-dir*r : sb+dir*r) + side*(k.y*r);

  outA = sa;
  outB = sb;
  outRadius = vec2(r-1.5, c.b.w);
  outTint = c.tint.rgb;
  gl_Position = vec4(p/push.size.xy*2.0-1.0, 0.0, 1.0);
  }
