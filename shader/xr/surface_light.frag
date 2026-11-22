#version 450
#extension GL_GOOGLE_include_directive : enable

layout(binding = 0) uniform sampler2D source;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 result;

#ifdef PALETTE
#include "lighting/tonemapping.glsl"

layout(binding = 1) uniform sampler2D previous;

layout(push_constant, std140) uniform Push {
  VideoSettings video;
  vec4          extra; // x: how much of this frame enters the running average (1 to reset)
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
      float weight = mix(0.65,1.35,cell.y);
      sum += linear*weight;
      weights += weight;
      }
    }
  // Light from a screen changes smoothly: average over time instead of following every frame
  vec3 prev = texelFetch(previous, ivec2(gl_FragCoord.xy), 0).rgb;
  result = vec4(mix(prev, sum/weights, push.extra.x), 1.0);
  }
#else
layout(push_constant, std140) uniform Push {
  vec2  offset;
  vec2  scale;
  float strength;
  uint  srgbTarget;
  float pad0, pad1;
  vec4  footprint; // full footprint width and depth (m), window width (m), curve radius (m, 0 flat)
  vec4  emitter;   // height of the window's light above the surface (m)
  } push;

void main() {
  vec2 p = push.offset+uv*push.scale;
  // Position on the surface, metres: across the window, and from the window's foot towards the viewer
  const float W = push.footprint.z, R = push.footprint.w;
  float x = (p.x-0.5)*push.footprint.x;
  float z = p.y*push.footprint.y;
  // Distance in front of the window's bottom edge, and position along it. A curved window's
  // edge is an arc, so its light follows the arc.
  float dist = z, along = x;
  if(R>0.0) {
    dist  = R-length(vec2(x,R-z));
    along = R*atan(x,R-z);
    }
  // A vertical screen h above a surface lights it with E ~ d*h/(d^2+dx^2+h^2)^2:
  // highest at d = h/sqrt(3) in front of the window, and spreading sideways into a
  // cone as distance from the screen grows.
  const float h  = max(push.emitter.x, 0.04);
  const float d  = max(dist, 0.0);
  const float dx = max(abs(along) - max(W, 0.1)*0.5, 0.0);
  const float r2 = d*d + dx*dx + h*h;
  const float E  = d*h/(r2*r2);
  const float Ep = 9.0/(16.0*sqrt(3.0)*h*h);
  const float near = smoothstep(-0.02, 0.02, dist);
  // Cone-shaped spread: starts around the window's width and fans out sideways with forward distance.
  const float halfSpan = max(W, 0.1)*0.5 + d*0.55;
  const float lateral = 1.0-smoothstep(0.60, 1.15, abs(along)/halfSpan);

  // Colour along the window, softly averaged across neighbouring bands
  const float u = clamp(along/max(W,0.1)+0.5, 0.0, 1.0);
  vec3 color = 0.5*textureLod(source,vec2(u,0.5),0.0).rgb;
  color += 0.25*textureLod(source,vec2(u-0.2,0.5),0.0).rgb;
  color += 0.25*textureLod(source,vec2(u+0.2,0.5),0.0).rgb;

  // Feather the detected tabletop boundary even when it clips the larger footprint.
  vec2 edge = min(uv,1.0-uv)*vec2(32.0,18.0);
  float support = smoothstep(0.5,2.0,min(edge.x,edge.y));
  vec3 light = color*(E/Ep)*near*lateral*support*push.strength;
  float alpha = max(max(light.r,light.g),light.b);
  // Borrowed sRGB images have UNORM Vulkan views. Supply encoded RGB to that
  // view so runtime sampling reconstructs linear light before compositing it.
  vec3 encoded = mix(1.055*pow(light,vec3(1.0/2.4))-0.055,12.92*light,lessThanEqual(light,vec3(0.0031308)));
  result = vec4(push.srgbTarget!=0u ? encoded : light,alpha);
  }
#endif
