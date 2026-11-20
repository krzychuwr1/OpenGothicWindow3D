#include "pfxobjects.h"

#include <Tempest/Log>
#ifdef OPENGOTHIC_XR
#include <Tempest/Application>
#include <Tempest/Except>
#endif
#include <cstring>

#include "graphics/sceneglobals.h"

#include "pfxbucket.h"
#include "particlefx.h"

using namespace Tempest;

PfxObjects::PfxObjects(WorldView& world, const SceneGlobals& scene, VisualObjects& visual)
  :world(world), scene(scene), visual(visual) {
  }

PfxObjects::~PfxObjects() {
  }

void PfxObjects::setViewerPos(const Vec3& pos) {
  viewerPos = pos;
  }

void PfxObjects::resetTicks() {
  lastUpdate = size_t(-1);
  }

void PfxObjects::tick(uint64_t ticks) {
  static bool disabled = false;
  if(disabled)
    return;

  if(lastUpdate>ticks) {
    lastUpdate = ticks;
    return;
    }

  uint64_t dt = ticks-lastUpdate;
  if(dt==0)
    return;

  for(auto& i:bucket)
    i.tick(dt,viewerPos);

  for(auto& i:bucket)
    i.buildSsbo();

  lastUpdate = ticks;
  }

bool PfxObjects::isInPfxRange(const Vec3& pos) const {
  auto dp = viewerPos-pos;
  return dp.quadLength()<viewRage*viewRage;
  }

void PfxObjects::preFrameUpdate(uint8_t fId, bool secondView) {
#ifdef OPENGOTHIC_XR
  frameSlot = fId;
  prevEyeView = lastEyeView[secondView];
  lastEyeView[secondView] = scene.view;
#endif
  for(auto i=bucket.begin(), end = bucket.end(); i!=end; ) {
    if(i->isEmpty()) {
      i = bucket.erase(i);
      } else {
      ++i;
      }
    }

  for(auto& i:bucket)
    i.preFrameUpdate(scene, fId);
  }

#ifdef OPENGOTHIC_XR
void PfxObjects::deferMotion() {
  // A partially updated history cannot be paired with the next submitted view.
  motionSnapshots = 0;
  motionRetryAfter = Application::tickCount()+3000;
  Log::e("SpaceWarp particle allocation failed, retrying in three seconds");
  }

void PfxObjects::snapshotParticles() {
  if(motionRetryAfter!=0 && Application::tickCount()<motionRetryAfter)
    return;
  try {
    for(auto& i:bucket)
      i.snapshotParticles();
    motionRetryAfter = 0;
    if(motionSnapshots<2)
      ++motionSnapshots;
    }
  catch(const std::bad_alloc&) {
    deferMotion();
    }
  }

void PfxObjects::drawMotion(Tempest::Encoder<Tempest::CommandBuffer>& cmd, Tempest::Size motionSize) {
  if(motionSnapshots<2)
    return;
  try {
    for(auto& i:bucket)
      i.drawMotion(cmd, scene, frameSlot, prevEyeView, motionSize);
    }
  catch(const std::bad_alloc&) {
    deferMotion();
    }
  catch(const std::system_error& e) {
    if(e.code()!=GraphicsErrc::OutOfVideoMemory && e.code()!=GraphicsErrc::OutOfHostMemory)
      throw;
    deferMotion();
    }
  }
#endif

void PfxObjects::drawGBuffer(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  for(auto& i:bucket)
    i.drawGBuffer(cmd, scene, fId);
  }

void PfxObjects::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, int layer) {
  for(auto& i:bucket)
    i.drawShadow(cmd, scene, fId, layer);
  }

void PfxObjects::drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  for(auto& i:bucket)
    i.drawTranslucent(cmd, scene, fId);
  }

PfxBucket& PfxObjects::getBucket(const ParticleFx &decl) {
  for(auto& i:bucket)
    if(&i.decl==&decl)
      return i;
  bucket.emplace_back(decl,*this,scene,visual);
  return bucket.back();
  }

PfxBucket& PfxObjects::getBucket(const Material& mat, const zenkit::VirtualObject& vob) {
  Tempest::Vec2 dimension {};
  if(auto decal = dynamic_cast<const zenkit::VisualDecal*>(vob.visual.get()))
    dimension = Tempest::Vec2{decal->dimension.x, decal->dimension.y};

  for(auto& i:spriteEmit)
    if(i.pfx->visMaterial==mat &&
       i.visualCamAlign==vob.sprite_camera_facing_mode &&
       i.zBias==vob.bias &&
       i.decalDim==dimension) {
      return getBucket(*i.pfx);
      }
  spriteEmit.emplace_back();
  auto& e = spriteEmit.back();

  e.visualCamAlign = vob.sprite_camera_facing_mode;
  e.zBias          = vob.bias;
  e.decalDim       = dimension;
  e.pfx.reset(new ParticleFx(mat,vob));
  return getBucket(*e.pfx);
  }

