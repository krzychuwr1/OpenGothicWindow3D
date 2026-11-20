#version 450
#extension GL_GOOGLE_include_directive : enable

layout(binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 result;

#ifdef PALETTE
#include "lighting/tonemapping.glsl"

layout(push_constant, std140) uniform Push {
  VideoSettings video;
  } push;

void main() {
  // Integrate a band over the full height. The surface receives broad colour
  // variation across the window, without readable characters or an upside-down scene.
  float band = floor(gl_FragCoord.x);
  vec3 sum = vec3(0.0);
  float weights = 0.0;
  for(int y=0; y<8; ++y) {
    for(int x=0; x<6; ++x) {
      // Fixed stratified offsets avoid aligning the sparse samples with one
      // repeating piece of scenery. Every frame uses the same pattern.
      vec2 jitter = fract(vec2(float(y)*0.75488+band*0.56984, float(x)*0.56984+band*0.75488));
      vec2 cell = (vec2(x,y)+0.2+0.6*jitter)/vec2(6.0,8.0);
      vec2 sampleUv = vec2((band+cell.x)/8.0,cell.y);
      vec3 display = gameTonemap(textureLod(source,sampleUv,0.0).rgb,push.video);
      vec3 linear = pow(display,vec3(2.2));
      // Give small bright emitters (fire and spells) enough influence to
      // change the broad spill, without copying their shapes onto the desk.
      float peak = max(max(linear.r,linear.g),linear.b);
      float weight = mix(0.65,1.35,cell.y)*(1.0+2.0*peak);
      sum += linear*weight;
      weights += weight;
      }
    }
  result = vec4(sum/weights,1.0);
  }
#else
layout(push_constant, std140) uniform Push {
  vec2 offset;
  vec2 scale;
  float strength;
  uint srgbTarget;
  } push;

void main() {
  vec2 p = push.offset+uv*push.scale;
  // Distant points receive a wider mixture of screen colours. Keep vertical
  // detail completely diffuse so light on a table never resembles a reflection.
  float spread = mix(0.12,0.38,p.y);
  vec3 color = 0.40*textureLod(source,vec2(p.x,0.5),0.0).rgb;
  color += 0.24*textureLod(source,vec2(p.x-spread,0.5),0.0).rgb;
  color += 0.24*textureLod(source,vec2(p.x+spread,0.5),0.0).rgb;
  color += 0.06*textureLod(source,vec2(p.x-2.0*spread,0.5),0.0).rgb;
  color += 0.06*textureLod(source,vec2(p.x+2.0*spread,0.5),0.0).rgb;
  float lateral = 1.0-smoothstep(0.28,1.0,abs(2.0*p.x-1.0));
  float falloff = smoothstep(0.0,0.10,p.y)*exp(-2.8*p.y)*(1.0-smoothstep(0.65,1.0,p.y));
  // Feather the detected tabletop boundary even when it clips the larger footprint.
  vec2 edge = min(uv,1.0-uv)*vec2(32.0,18.0);
  float support = smoothstep(0.5,2.0,min(edge.x,edge.y));
  vec3 light = color*lateral*falloff*support*push.strength;
  float alpha = max(max(light.r,light.g),light.b);
  // Borrowed sRGB images have UNORM Vulkan views. Supply encoded RGB to that
  // view so runtime sampling reconstructs linear light before compositing it.
  vec3 encoded = mix(1.055*pow(light,vec3(1.0/2.4))-0.055,12.92*light,lessThanEqual(light,vec3(0.0031308)));
  result = vec4(push.srgbTarget!=0u ? encoded : light,alpha);
  }
#endif
