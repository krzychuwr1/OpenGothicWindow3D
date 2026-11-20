vec4 diffuseTex() {
#if !defined(SIMPLE_MAT) && (MESH_TYPE!=T_PFX)
  ivec2 texAniMapDirPeriod = bucket[bucketId].texAniMapDirPeriod;
  float alphaWeight        = bucket[bucketId].alphaWeight;
#else
  ivec2 texAniMapDirPeriod = ivec2(0);
  float alphaWeight        = 1;
#endif

#if !defined(SIMPLE_MAT)
  vec2 texAnim = vec2(0);
  {
    // FIXME: this not suppose to run for every-single material
    if(texAniMapDirPeriod.x!=0) {
      uint fract = scene.tickCount32 % abs(texAniMapDirPeriod.x);
      texAnim.x  = float(fract)/float(texAniMapDirPeriod.x);
      }
    if(texAniMapDirPeriod.y!=0) {
      uint fract = scene.tickCount32 % abs(texAniMapDirPeriod.y);
      texAnim.y  = float(fract)/float(texAniMapDirPeriod.y);
      }
  }
  const vec2 uv = shInp.uv + texAnim;
#else
  const vec2 uv = shInp.uv;
#endif

#if defined(BINDLESS)
  nonuniformEXT uint tId = bucketId;
#else
  const         uint tId = 0;
#endif

#if defined(DIFFUSE_GRAD_SCALE)
  // Match the colour pass's material mip at the smaller motion raster's pixel centres.
  vec4 tex = textureGrad(sampler2D(textureMain[tId], samplerMain),uv,
                         dFdx(uv)*DIFFUSE_GRAD_SCALE.x,dFdy(uv)*DIFFUSE_GRAD_SCALE.y);
#else
  vec4 tex = texture(sampler2D(textureMain[tId], samplerMain),uv);
#endif

#if !defined(SIMPLE_MAT)
  tex.a *= alphaWeight;
#endif

  // return vec4(1,1,1, tex.a);
  return tex;
  }
