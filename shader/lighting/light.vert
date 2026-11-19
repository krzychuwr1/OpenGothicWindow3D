#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

#include "scene.glsl"

out gl_PerVertex {
  vec4 gl_Position;
  };

layout(push_constant, std140) uniform Pbo {
  vec3  origin; //lwc
  } push;

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };

layout(binding = 4, std430) readonly buffer SsboLighting {
  LightSource lights[];
  };

layout(location = 0) out vec4 cenPosition;
layout(location = 1) out vec3 color;
layout(location = 2) out flat uint lightId;

vec3 v[] = {
  {-1,-1,-1},
  { 1,-1,-1},
  { 1, 1,-1},
  {-1, 1,-1},

  {-1,-1, 1},
  { 1,-1, 1},
  { 1, 1, 1},
  {-1, 1, 1},
  };

bool testFrustrum(in vec3 at, in float R){
  for(int i=0; i<6; i++) {
    if(dot(scene.frustrum[i],vec4(at,1.0))<-R)
      return false;
    }
  return true;
  }

void main(void) {
  LightSource light = lights[gl_InstanceIndex];

  if(!testFrustrum(light.pos,light.range)) {
    // skip invisible lights, make sure that they don't turn into FQS
    gl_Position = vec4(0.0,0.0,-1.0,1.0);
    cenPosition = vec4(0.0);
    color       = vec3(0.0);
    return;
    }

  // Clip-space corners of the light's bounding cube
  vec4 c[8];
  int  neg = 0;
  for(int i=0; i<8; ++i) {
    c[i] = scene.viewProject*vec4(light.pos + v[i]*light.range, 1.0);
    if(c[i].z<0.0)
      neg++;
    }

  const vec3 inPos = v[gl_VertexIndex];
  vec4 pos = vec4(0);
  if(neg==8) {
    // entirely in front of the near plane: nothing to light
    pos = vec4(uintBitsToFloat(0x7fc00000));
    }
  else if(neg>0) {
    // The cube crosses the near plane: draw its screen rectangle (clipped at the near plane)
    // instead of the whole screen. Interpolating in clip space keeps the clip points exact.
    vec2 mn = vec2( 1e9);
    vec2 mx = vec2(-1e9);
    for(int i=0; i<8; ++i) {
      if(c[i].z>=0.0) {
        vec2 p = c[i].xy/c[i].w;
        mn = min(mn, p);
        mx = max(mx, p);
        }
      }
    const ivec2 edges[12] = {ivec2(0,1),ivec2(1,2),ivec2(2,3),ivec2(3,0),
                             ivec2(4,5),ivec2(5,6),ivec2(6,7),ivec2(7,4),
                             ivec2(0,4),ivec2(1,5),ivec2(2,6),ivec2(3,7)};
    for(int e=0; e<12; ++e) {
      const vec4 a = c[edges[e].x];
      const vec4 b = c[edges[e].y];
      if((a.z>=0.0) == (b.z>=0.0))
        continue;
      const vec4 p = mix(a, b, a.z/(a.z-b.z));
      if(p.w<=0.0) {
        // degenerate clip: fall back to the whole screen
        mn = vec2(-1);
        mx = vec2( 1);
        break;
        }
      mn = min(mn, p.xy/p.w);
      mx = max(mx, p.xy/p.w);
      }
    mn = clamp(mn, vec2(-1), vec2(1));
    mx = clamp(mx, vec2(-1), vec2(1));
    // same corner order and winding as the full-screen quad this replaces
    const vec2 fsq = vec2(inPos.x, -inPos.y);
    if(gl_VertexIndex>=4)
      pos = vec4(uintBitsToFloat(0x7fc00000)); else
      pos = vec4(fsq.x<0.0 ? mn.x : mx.x, fsq.y<0.0 ? mn.y : mx.y, 0.0, 1.0);
    } else {
    pos = scene.viewProject*vec4(light.pos+inPos*light.range, 1.0);
    }

  //const vec3 origin = vec3(38983.9336, 4080.52637, -1888.59839);
  gl_Position = pos;
  cenPosition = vec4(light.pos,light.range);
  color       = light.color;
  lightId     = gl_InstanceIndex;
  }
