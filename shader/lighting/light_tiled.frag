#version 460
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable

// Point lights in one full-screen pass: the G-buffer is read once per pixel and only the
// lights binned into the pixel's screen tile are summed. Same light model as light.frag.

#include "lighting/tonemapping.glsl"
#include "scene.glsl"
#include "common.glsl"

layout(location = 0) out vec4 outColor;

layout(push_constant, std430) uniform Pbo {
  vec3  origin; // lwc
  uint  tilesX;
  uint  tileSize;
  uint  sliceStride;
  float ends[3];
  } push;

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding = 1) uniform texture2D  gbufDiffuse;
layout(binding = 2) uniform utexture2D gbufNormal;
layout(binding = 3) uniform texture2D  depth;

layout(binding = 4, std430) readonly buffer SsboLighting {
  LightSource lights[];
  };

// Per tile: (offset, count) into the index list that follows the tile table
layout(binding = 12, std430) readonly buffer SsboTiles {
  uint tileData[];
  };

void main() {
  const ivec2 fragCoord = ivec2(gl_FragCoord.xy);
  const float z         = texelFetch(depth, fragCoord, 0).x;
  if(z>=1.0)
    discard;

  const uvec2 tile  = uvec2(fragCoord)/push.tileSize;
  const uint  slice = uint(z>=push.ends[0]) + uint(z>=push.ends[1]) + uint(z>=push.ends[2]);
  const uint  t     = slice*push.sliceStride + tile.y*push.tilesX + tile.x;
  const uint  first = tileData[t*2+0];
  const uint  count = tileData[t*2+1];
  if(count==0)
    discard;

  const vec2 scr = (gl_FragCoord.xy/vec2(textureSize(depth,0)))*2.0-1.0;
  vec4 pos = scene.viewProjectLwcInv*vec4(scr.x,scr.y,z,1.0);
  pos.xyz /= pos.w;
  pos.xyz += push.origin;

  const vec3 normal = normalFetch(gbufNormal, fragCoord);

  vec3 sum = vec3(0);
  for(uint i=0; i<count; ++i) {
    const LightSource light = lights[tileData[first+i]];
    const vec3  ldir           = pos.xyz-light.pos;
    const float distanceSquare = dot(ldir,ldir);
    const float factor         = distanceSquare / (light.range*light.range);
    if(factor>1.0)
      continue;
    const float smoothFactor   = max(1.0 - factor * factor, 0.0);
    const float lambert        = max(0.0,-dot(normalize(ldir),normal));
    const float l              = (lambert/max(factor, 0.005)) * (smoothFactor*smoothFactor);
    if(l<=0.0)
      continue;
    sum += light.color * l;
    }
  if(sum==vec3(0))
    discard;

  const vec3 d      = texelFetch(gbufDiffuse, fragCoord, 0).xyz;
  const vec3 linear = textureAlbedo(d.rgb);
  outColor = vec4(linear * sum * max(1.0, scene.exposure) * Fd_Lambert * 0.25, 0.0);
  }
