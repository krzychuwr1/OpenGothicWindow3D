#version 450

layout(push_constant) uniform Trial { vec4 gainAndOffset; } trial;
layout(binding = 0) uniform sampler2D motion;
layout(location = 0) out vec4 result;

void main() {
  ivec2 at = ivec2(gl_FragCoord.xy);
  vec3 v = texelFetch(motion, at, 0).xyz * trial.gainAndOffset.xyz;
  // A known positive X (trial 8) or Y (trial 9) displacement verifies the
  // display sign and synthesized frames. Leave the frame-ID strip unaffected.
  if(trial.gainAndOffset.w==1.0) {
    // Same 64-unit grid as the two stationary colour bars in paintEyeOverlay.
    // Preserve actual scene vectors everywhere outside their black backing.
    vec2 cell = (vec2(at)+0.5)*64.0/vec2(textureSize(motion,0));
    bool patchRow = (cell.y>=24.0 && cell.y<28.0) || (cell.y>=48.0 && cell.y<52.0);
    if(cell.x>=14.0 && cell.x<23.0 && patchRow)
      v = vec3(cell.x>=18.0 && cell.x<21.0 ? 0.08 : 0.0,0.0,0.0);
    } else if(at.y > textureSize(motion,0).y/4 && at.y < 3*textureSize(motion,0).y/4)
    v.xy += vec2(max(trial.gainAndOffset.w,0.0),max(-trial.gainAndOffset.w,0.0));
  result = vec4(v,0);
  }
