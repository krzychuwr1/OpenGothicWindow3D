#include "pose.h"

#include <Tempest/Log>

#include "utils/string_frm.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "game/serialize.h"
#include "skeleton.h"
#include "animmath.h"

#include <cmath>

using namespace Tempest;

Pose::Pose() {
  lay.reserve(4);
  }

uint8_t Pose::calcAniComb(const Vec3& dpos, float rotation) {
  float   l   = std::sqrt(dpos.x*dpos.x+dpos.z*dpos.z);

  float   dir = 180.f*std::atan2(dpos.z,dpos.x)/float(M_PI);
  float   aXZ = (rotation-dir);
  float   aY  = -std::atan2(dpos.y,l)*180.f/float(M_PI);

  uint8_t cx  = (aXZ<-30.f) ? 0 : (aXZ<=30.f ? 1 : 2);
  uint8_t cy  = (aY <-45.f) ? 0 : (aY <=45.f ? 1 : 2);

  // sides angle: +/- 30 height angle: +/- 45
  return uint8_t(1u+cy*3u+cx);
  }

uint8_t Pose::calcAniCombVert(const Vec3& dpos) {
  float   l  = std::sqrt(dpos.x*dpos.x+dpos.z*dpos.z);
  float   aY = 180.f*std::atan2(dpos.y,l)/float(M_PI);
  uint8_t cy = (aY <-25.f) ? 0 : (aY <=25.f ? 1 : 2);

  if(dpos.y<-50)
    cy = 0;
  if(dpos.y>50)
    cy = 2;

  // height angle: +/- 25
  return uint8_t(cy+1u);
  }

void Pose::save(Serialize &fout) {
  uint8_t sz=uint8_t(lay.size());
  fout.write(sz);
  for(auto& i:lay) {
    fout.write(i.seq->name,i.sAnim,i.bs,i.sBlend);
    }
  fout.write(lastUpdate);
  fout.write(combo.bits);
  fout.write(rotation ? rotation->name : "");
  fout.write(itemUseSt,itemUseDestSt);
  fout.write(headRotX,headRotY);

  for(auto& i:hasSamples)
    fout.write(uint8_t(i));
  for(auto& i:base)
    fout.write(i);
  for(auto& i:prev)
    fout.write(i);
  for(auto& i:tr)
    fout.write(i);
  }

void Pose::load(Serialize &fin, const AnimationSolver& solver) {
  std::string name;
  uint8_t     sz = uint8_t(lay.size());

  fin.read(sz);
  lay.resize(sz);
  for(auto& i:lay) {
    fin.read(name,i.sAnim,i.bs);
    if(fin.version()>43)
      fin.read(i.sBlend);
    i.seq = solver.solveFrm(name);
    }
  fin.read(lastUpdate);
  fin.read(combo.bits);
  removeIf(lay,[](const Layer& l){
    return l.seq==nullptr;
    });
  fin.read(name);
  for(auto& i:lay) {
    if(i.seq->name==name)
      rotation = i.seq;
    }
  fin.read(itemUseSt,itemUseDestSt);
  hasEvents = 0;
  isFlyCombined = 0;
  hasTransitions = 0;
  for(auto& i:lay)
    onAddLayer(i);
  fin.read(headRotX,headRotY);
  needToUpdate = true;

  numBones = skeleton==nullptr ? 0 : skeleton->nodes.size();
  for(auto& i:hasSamples)
    fin.read(reinterpret_cast<uint8_t&>(i));
  for(auto& i:base)
    fin.read(i);
  for(auto& i:prev)
    fin.read(i);
  for(auto& i:tr)
    fin.read(i);
  }

void Pose::setFlags(Pose::Flags f) {
  flag         = f;
  needToUpdate = true;
  }

BodyState Pose::bodyState() const {
  uint32_t b = BS_NONE;
  for(auto& i:lay)
    b = std::max(b,uint32_t(i.bs&(BS_MAX | BS_FLAG_MASK)));
  return BodyState(b);
  }

bool Pose::hasState(BodyState s) const {
  for(auto& i:lay)
    if(i.bs==s)
      return true;
  return false;
  }

bool Pose::hasStateFlag(BodyState f) const {
  for(auto& i:lay)
    if((i.bs & (BS_FLAG_MASK | BS_MOD_MASK))==f)
      return true;
  return false;
  }

void Pose::setSkeleton(const Skeleton* sk) {
  if(skeleton==sk)
    return;
  skeleton = sk;
  for(auto& i:tr)
    i.identity();
  if(skeleton!=nullptr) {
    numBones = skeleton->tr.size();
    } else {
    numBones = 0;
    }
  for(auto& i:hasSamples)
    i = S_None;
  trY          = skeleton!=nullptr ? skeleton->rootTr.y : 0;
  needToUpdate = true;
  if(lay.size()>0) //TODO
    Log::d("WARNING: ",__func__," animation adjustment is not implemented");
  lay.clear();

  if(skeleton!=nullptr)
    mkSkeleton(Matrix4x4::mkIdentity());
  }

bool Pose::startAnim(const AnimationSolver& solver, const Animation::Sequence *sq, uint8_t comb, BodyState bs,
                     StartHint hint, uint64_t tickCount) {
  if(sq==nullptr)
    return false;
  // NOTE: zero stands for no-comb, other numbers are comb-index+1
  comb = std::min<uint8_t>(comb, uint8_t(sq->comb.size()));

  const bool force = (hint&Force);

  for(auto& i:lay)
    if(i.seq->layer==sq->layer) {
      const bool hasNext   = !i.seq->next.empty();
      const bool finished  = i.seq->isFinished(tickCount,i.sAnim,combo.len()) && !hasNext && (i.seq->animCls!=Animation::Loop);
      const bool interrupt = force || i.seq->canInterrupt(tickCount,i.sAnim,combo.len());
      if(i.seq==sq && i.comb==comb && !finished) {
#if defined(OPENGOTHIC_XR)
        if(flag==NoTranslation && i.bs!=bs)
          needToUpdate = true;
#endif
        i.bs = bs;
        return true;
        }
      if(!interrupt && !finished)
        return false;
      if(i.bs==BS_ITEMINTERACT) {
        stopItemStateAnim(solver,tickCount);
        return false;
        }
      //WA: Emulate behaviour of vanilla: lurker skips transition animations when entering the water
      const bool swimFix = (i.sAnim==tickCount && i.bs==BS_SWIM && bs==BS_SWIM);
      const Animation::Sequence* tr=nullptr;
      if(i.seq->shortName!=nullptr && sq->shortName!=nullptr && !swimFix) {
        string_frm tansition("T_",i.seq->shortName,"_2_",sq->shortName);
        tr = solver.solveFrm(tansition);
        }
      if(tr==nullptr && sq->shortName!=nullptr && i.bs==BS_STAND) {
        string_frm tansition("T_STAND_2_",sq->shortName);
        tr = solver.solveFrm(tansition);
        }
      if(tr==nullptr && i.seq->shortName!=nullptr && bs==BS_STAND) {
        string_frm tansition("T_",i.seq->shortName,"_2_STAND");
        tr = solver.solveFrm(tansition);
        bs = tr ? i.bs : bs;
        }
      onRemoveLayer(i);
      i.seq      = tr ? tr : sq;
      i.sAnim    = tickCount;
      i.sBlend   = 0;
      i.comb     = comb;
      i.bs       = bs;
      onAddLayer(i);
      return true;
      }
  addLayer(sq,bs,comb,tickCount);
  return true;
  }

bool Pose::stopAnim(std::string_view name) {
  bool done=false;
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    bool rm = (name.empty() || lay[i].seq->name==name);
    if(itemUseSt!=0 && lay[i].bs==BS_ITEMINTERACT)
      rm = false;

    if(!rm) {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      } else {
      onRemoveLayer(lay[i]);
      done=true;
      }
    }
  lay.resize(ret);
  return done;
  }

bool Pose::stopWalkAnim() {
  bool done=false;
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    if(lay[i].bs!=BS_RUN && lay[i].bs!=BS_SPRINT && lay[i].bs!=BS_SNEAK && lay[i].bs!=BS_WALK) {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      } else {
      onRemoveLayer(lay[i]);
      done=true;
      }
    }
  lay.resize(ret);
  return done;
  }

void Pose::interrupt() {
  size_t ret=0;
  for(size_t i=0;i<lay.size();++i) {
    if(BS_FLAG_INTERRUPTABLE & lay[i].bs) {
      onRemoveLayer(lay[i]);
      } else {
      if(ret!=i)
        lay[ret] = lay[i];
      ret++;
      }
    }
  lay.resize(ret);
  }

void Pose::stopAllAnim() {
  for(auto& i:lay)
    onRemoveLayer(i);
  lay.clear();
  }

void Pose::processLayers(AnimationSolver& solver, uint64_t tickCount) {
  if(hasTransitions==0)
    return;

  size_t ret    = 0;
  bool   doSort = false;
  for(size_t i=0; i<lay.size(); ++i) {
    auto& l = lay[i];
    if(l.seq->animCls==Animation::Transition && l.seq->isFinished(tickCount,l.sAnim,combo.len())) {
      auto next = solveNext(solver,l);
      if(next!=l.seq) {
        needToUpdate = true;
        onRemoveLayer(l);

        if(next!=nullptr) {
          if(l.seq==rotation)
            rotation = next;
          doSort  = l.seq->layer!=next->layer;
          auto bs = (l.bs & BS_MAX);
          // WA for:
          // 1. for swampshark, "T_STUMBLEB" has next animationn: "S_FISTRUN"
          // 2. for some of jump-back animnations, also 'next' is not empty
          if(bs==BS_STUMBLE || bs==BS_PARADE)
            l.bs = BS_NONE;
          l.seq   = next;
          l.sAnim = tickCount;
          onAddLayer(l);
          ret++;
          }
        continue;
        }
      }
    if(ret!=i)
      lay[ret] = lay[i];
    ret++;
    }
  lay.resize(ret);

  if(doSort) {
    std::sort(lay.begin(),lay.end(),[](const Layer& a,const Layer& b){
      return a.seq->layer<b.seq->layer;
      });
    }
  }

bool Pose::update(uint64_t tickCount, bool force) {
  // Tracked arms move every frame, whatever the animation does (held frames included)
  const bool armIkDirty = hasArmTargets() || std::exchange(armIk.settle,false);
  if(lay.size()==0) {
    const bool ret = needToUpdate || armIkDirty;
    if(needToUpdate || lastUpdate==0 || armIkDirty)
      mkSkeleton(pos);
    needToUpdate = false;
    lastUpdate   = tickCount;
    return ret;
    }

  bool needMkSkeleton = false;
  if(lastUpdate!=tickCount || force) {
    bool holdFrame = false;
    for(auto& i:lay) {
      const Animation::Sequence* seq = i.seq;
      if(0<i.comb && i.comb<=i.seq->comb.size()) {
        if(auto sx = i.seq->comb[size_t(i.comb-1)])
          seq = sx;
        }

      auto&        d         = *seq->data;
      const size_t numFrames = d.numFrames;
      if(numFrames==1 && !needToUpdate)
        continue; //mobsi

      needMkSkeleton |= updateFrame(*seq,i.bs,i.sBlend,lastUpdate,i.sAnim,tickCount);
#if defined(OPENGOTHIC_XR)
      if(needMkSkeleton && numFrames==1 && flag==NoTranslation && lay.size()==1)
        holdFrame = canHoldFrame(*seq,i,tickCount);
#endif
      }
    lastUpdate   = tickCount;
    needToUpdate = needMkSkeleton && !holdFrame;
    }

  if(needMkSkeleton || force || armIkDirty) {
    mkSkeleton(pos);
    return true;
    }
  return false;
  }

#if defined(OPENGOTHIC_XR)
bool Pose::canHoldFrame(const Animation::Sequence& seq, const Layer& layer, uint64_t now) const {
  const auto elapsed = now-layer.sAnim;
  if(now<layer.sAnim || elapsed<layer.sBlend || elapsed-layer.sBlend<std::max(seq.blendIn,seq.blendOut))
    return false;
  const auto& d = *seq.data;
  if(d.samples.size()!=d.nodeIndex.size())
    return false;
  for(size_t i=0; i<d.samples.size(); ++i) {
    const auto bone = d.nodeIndex[i];
    if(bone>=numBones || hasSamples[bone]!=S_Valid)
      return false;
    // Only the linear branch of slerp(q,q,t) is independent of the fractional frame.
    const auto& q = d.samples[i].rotation;
    if(!(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w > 1.f-std::numeric_limits<float>::epsilon()))
      return false;
    }
  return true;
  }
#endif

bool Pose::updateFrame(const Animation::Sequence &s, BodyState bs, uint64_t sBlend,
                       uint64_t barrier, uint64_t sTime, uint64_t now) {
  auto&        d         = *s.data;
  const size_t idSize    = d.nodeIndex.size();
  const size_t numFrames = d.numFrames;
  if(numFrames==0 || idSize==0 || d.samples.size()%idSize!=0)
    return false; // error

  (void)barrier;
  now = now-sTime;

  float    fpsRate = d.fpsRate;
  uint64_t frame   = uint64_t(float(now)*fpsRate);
  uint64_t frameA  = frame/1000;
  uint64_t frameB  = frame/1000+1; //next

  float    a       = float(frame%1000)/1000.f;

  if(s.animCls==Animation::Loop) {
    frameA%=d.numFrames;
    frameB%=d.numFrames;
    } else {
    frameA = std::min<uint64_t>(frameA,d.numFrames-1);
    frameB = std::min<uint64_t>(frameB,d.numFrames-1);
    }

  if(s.reverse) {
    frameA = d.numFrames-1-frameA;
    frameB = d.numFrames-1-frameB;
    }

  auto* sampleA = &d.samples[size_t(frameA*idSize)];
  auto* sampleB = &d.samples[size_t(frameB*idSize)];

  const uint64_t blendMax = std::max(s.blendOut,s.blendIn);
  const uint64_t blend    = std::max<uint64_t>(0, now-sBlend);

  for(size_t i=0; i<idSize; ++i) {
    size_t idx = d.nodeIndex[i];
    if(idx>=numBones)
      continue;
    auto smp = mix(sampleA[i],sampleB[i],a);
    if(i==0) {
      if(bs==BS_CLIMB)
        smp.position.y = trY;
      else if(bs==BS_SWIM || bs==BS_DIVE)
        smp.position.y = trY;
      else if(bs==BS_JUMP) //else if(s.isFly())
        smp.position.y = trY;
      }

    switch(hasSamples[idx]) {
      case S_None:
        hasSamples[idx] = S_Old;
        base      [idx] = smp;
        break;
      case S_Old:
        hasSamples[idx] = S_Valid;
        prev      [idx] = base[idx];
        [[fallthrough]];
      case S_Valid:
        if(blend < blendMax) {
          float a2 = float(blend)/float(blendMax);
          assert(0.f<=a2 && a2<=1.f);
          base[idx] = mix(prev[idx],smp,a2);
          } else {
          prev[idx] = smp;
          base[idx] = smp;
          }
        break;
      }
    }
  return true;
  }

void Pose::mkSkeleton(const Tempest::Matrix4x4& mt) {
  if(skeleton==nullptr)
    return;
  Matrix4x4 m = mt;
  m.translate(mkBaseTranslation());
  if(skeleton->ordered)
    implMkSkeleton(m); else
    implMkSkeleton(m,size_t(-1));
  if(hasArmTargets())
    applyArmIk(mt);
  }

void Pose::implMkSkeleton(const Matrix4x4 &mt) {
  if(skeleton==nullptr)
    return;
  auto& nodes      = skeleton->nodes;
  auto  BIP01_HEAD = skeleton->BIP01_HEAD;
  for(size_t i=0; i<nodes.size(); ++i) {
    size_t parent = nodes[i].parent;
    auto   mat    = hasSamples[i] ? mkMatrix(base[i]) : nodes[i].tr;

    if(parent<Resources::MAX_NUM_SKELETAL_NODES)
      tr[i] = tr[parent]*mat; else
      tr[i] = mt*mat;

    if(i==BIP01_HEAD && (headRotX!=0 || headRotY!=0)) {
      Matrix4x4& m = tr[i];
      m.rotateOY(headRotY);
      m.rotateOX(headRotX);
      }
    }
  }

void Pose::implMkSkeleton(const Tempest::Matrix4x4 &mt, size_t parent) {
  if(skeleton==nullptr)
    return;
  auto& nodes = skeleton->nodes;
  for(size_t i=0;i<nodes.size();++i){
    if(nodes[i].parent!=parent)
      continue;
    auto mat = hasSamples[i] ? mkMatrix(base[i]) : nodes[i].tr;
    tr[i] = mt*mat;
    implMkSkeleton(tr[i],i);
    }
  }

namespace {

Vec3 bonePos(const Matrix4x4& m) {
  return Vec3(m.at(3,0), m.at(3,1), m.at(3,2));
  }

Vec3 boneAxis(const Matrix4x4& m, int c) {
  return Vec3(m.at(c,0), m.at(c,1), m.at(c,2));
  }

float component(const Vec3& v, int k) {
  return k==0 ? v.x : (k==1 ? v.y : v.z);
  }

// World bone matrix turned by `angle` radians around the unit `axis` through `pivot`: T(pivot)*R*T(-pivot)*m
void rotateAround(Matrix4x4& m, const Vec3& pivot, const Vec3& axis, float angle) {
  const float c = std::cos(angle), s = std::sin(angle);
  auto rot = [&](const Vec3& v) {
    return v*c + Vec3::crossProduct(axis,v)*s + axis*(Vec3::dotProduct(axis,v)*(1.f-c));
    };
  for(int k=0; k<3; ++k) {
    const Vec3 v = rot(boneAxis(m,k));
    m.set(k,0,v.x);
    m.set(k,1,v.y);
    m.set(k,2,v.z);
    }
  const Vec3 p = pivot + rot(bonePos(m)-pivot);
  m.set(3,0,p.x);
  m.set(3,1,p.y);
  m.set(3,2,p.z);
  }

// Shortest rotation around `pivot` that turns direction `from` onto `to`
void rotateOnto(Matrix4x4& m, const Vec3& pivot, const Vec3& from, const Vec3& to) {
  const Vec3  axis = Vec3::crossProduct(from,to);
  const float s    = axis.length();
  if(s<=1e-6f*from.length()*to.length())
    return;
  rotateAround(m, pivot, axis/s, std::atan2(s,Vec3::dotProduct(from,to)));
  }

}

Matrix4x4 Pose::localMatrix(size_t i) const {
  return hasSamples[i] ? mkMatrix(base[i]) : skeleton->nodes[i].tr;
  }

void Pose::calibrateArms() {
  const auto& nodes = skeleton->nodes;
  const auto& rest  = skeleton->tr; // bind pose, model space
  armIk.skeleton = skeleton;
  armIk.finger.assign(nodes.size(),0);

  auto descends = [&](size_t i, size_t ancestor) {
    for(size_t n=0; n<nodes.size() && i<nodes.size(); ++n) {
      i = nodes[i].parent;
      if(i==ancestor)
        return true;
      }
    return false;
    };

  for(size_t h=0; h<2; ++h) {
    ArmChain& c = armIk.chain[h];
    c = ArmChain();
    const std::string side = h==0 ? "BIP01 L" : "BIP01 R";
    const size_t upper = skeleton->findNode(side+" UPPERARM");
    const size_t fore  = skeleton->findNode(side+" FOREARM");
    const size_t hand  = skeleton->findNode(side+" HAND");
    if(upper>=nodes.size() || fore>=nodes.size() || hand>=nodes.size() ||
       nodes[fore].parent!=upper || nodes[hand].parent!=fore) {
      Log::d("arm tracking: no ",side," arm chain in ",skeleton->name());
      continue;
      }

    // Biped names: FINGER0* is the thumb (kept animated), FINGER1 the index finger
    const std::string thumbName = side+" FINGER0", indexName = side+" FINGER1";
    size_t thumb = size_t(-1), finger = size_t(-1);
    for(size_t i=0; i<nodes.size(); ++i) {
      const auto& name = nodes[i].name;
      if(name.find("FINGER")==std::string::npos || !descends(i,hand))
        continue;
      if(name==thumbName)
        thumb = i;
      if(name.starts_with(thumbName))
        continue;
      armIk.finger[i] = uint8_t(1+h);
      if(name==indexName || (finger>=nodes.size() && nodes[i].parent==hand))
        finger = i;
      }

    // Calibration frame in hand bone space: towards the finger root (else along the forearm) and
    // towards the thumb root (else the back of the hand, up in the palms-down bind pose)
    Matrix4x4 toHand = rest[hand];
    toHand.inverse();
    auto local = [&](Vec3 p) { toHand.project(p); return p; };
    const Vec3 hp     = bonePos(rest[hand]);
    const Vec3 distal = local(finger<nodes.size() ? bonePos(rest[finger]) : hp*2.f-bonePos(rest[fore]));
    c.byThumb         = thumb<nodes.size();
    const Vec3 second = local(c.byThumb ? bonePos(rest[thumb]) : hp+Vec3(0,1,0));
    c.frame[0] = Vec3::normalize(distal);
    c.frame[1] = Vec3::normalize(second-c.frame[0]*Vec3::dotProduct(second,c.frame[0]));
    c.frame[2] = Vec3::crossProduct(c.frame[0],c.frame[1]);
    if(c.frame[0].quadLength()<0.5f || c.frame[1].quadLength()<0.5f)
      continue;
    c.upper = upper;
    c.fore  = fore;
    c.hand  = hand;
    Log::d("arm tracking: ",side," arm, finger root: ",finger<nodes.size() ? nodes[finger].name : "none",
           ", thumb: ",c.byThumb ? nodes[thumb].name : "none");
    }

  // Model space X points to the right arm when the right shoulder has the larger X
  const auto& l = armIk.chain[0];
  const auto& r = armIk.chain[1];
  if(l.upper<nodes.size() && r.upper<nodes.size())
    armIk.rightSide = bonePos(rest[r.upper]).x>=bonePos(rest[l.upper]).x ? 1.f : -1.f;
  else if(r.upper<nodes.size())
    armIk.rightSide = bonePos(rest[r.upper]).x>=0.f ? 1.f : -1.f;
  else if(l.upper<nodes.size())
    armIk.rightSide = bonePos(rest[l.upper]).x>=0.f ? -1.f : 1.f;
  }

void Pose::applyArmIk(const Matrix4x4& obj) {
  if(armIk.skeleton!=skeleton)
    calibrateArms();
  const auto&  nodes = skeleton->nodes;
  const size_t head  = skeleton->BIP01_HEAD;
  if(head>=nodes.size())
    return;

  const Vec3 headPos = bonePos(tr[head]);
  // Body frame -> object space -> world (direction)
  auto world = [&](const Vec3& v) {
    return boneAxis(obj,0)*(armIk.rightSide*v.x) + boneAxis(obj,1)*v.y + boneAxis(obj,2)*v.z;
    };
  const Vec3 down = -Vec3::normalize(boneAxis(obj,1));

  for(size_t h=0; h<2; ++h) {
    const ArmChain&  c = armIk.chain[h];
    const ArmTarget& t = armIk.target[h];
    const float      w = armIk.weight[h];
    if(w<=0.f || c.upper>=nodes.size())
      continue;

    const Vec3  S  = bonePos(tr[c.upper]);
    const Vec3  F  = bonePos(tr[c.fore]);
    const Vec3  H  = bonePos(tr[c.hand]);
    const float l1 = (F-S).length(), l2 = (H-F).length();
    if(l1<=1e-3f || l2<=1e-3f)
      continue;

    // The wrist goal, blended from the animated hand and clamped to the arm's reach
    Vec3  goal = H + (headPos+world(t.wrist)-H)*w;
    Vec3  dir  = goal-S;
    float dist = dir.length();
    if(dist<=1e-3f)
      continue;
    dir = dir/dist;
    const float reach = 0.999f*(l1+l2);
    dist = std::clamp(dist, std::min(std::abs(l1-l2)+0.01f*(l1+l2),reach), reach);
    goal = S + dir*dist;

    // Elbow: down and away from the body; from the animated elbow while fading
    auto across = [&](const Vec3& v) { return v - dir*Vec3::dotProduct(v,dir); };
    const size_t clavicle = nodes[c.upper].parent;
    const Vec3   outward  = clavicle<nodes.size() ? Vec3::normalize(S-bonePos(tr[clavicle])) : Vec3();
    const Vec3   pole     = Vec3::normalize(across(down + outward*0.5f));
    Vec3 bend = Vec3::normalize(across(Vec3::normalize(across(F-S))*(1.f-w) + pole*w));
    if(bend.quadLength()<0.5f)
      bend = pole;
    const float a = (l1*l1 - l2*l2 + dist*dist)/(2.f*dist);
    const float b = std::sqrt(std::max(l1*l1-a*a, 0.f));
    const Vec3  E = S + dir*a + bend*b;

    // Upper arm onto the elbow, rolled so the elbow's hinge bends towards the goal
    rotateOnto(tr[c.upper], S, F-S, E-S);
    tr[c.fore] = tr[c.upper]*localMatrix(c.fore);
    tr[c.hand] = tr[c.fore]*localMatrix(c.hand);
    {
      const Vec3 axis  = Vec3::normalize(bonePos(tr[c.fore])-S);
      const Vec3 elbow = bonePos(tr[c.fore]);
      Vec3 p1 = bonePos(tr[c.hand])-elbow, p2 = goal-elbow;
      p1 = p1 - axis*Vec3::dotProduct(p1,axis);
      p2 = p2 - axis*Vec3::dotProduct(p2,axis);
      const float eps = 1e-4f*l2*l2;
      if(p1.quadLength()>eps && p2.quadLength()>eps) {
        rotateAround(tr[c.upper], S, axis, std::atan2(Vec3::dotProduct(axis,Vec3::crossProduct(p1,p2)),Vec3::dotProduct(p1,p2)));
        tr[c.fore] = tr[c.upper]*localMatrix(c.fore);
        tr[c.hand] = tr[c.fore]*localMatrix(c.hand);
        }
    }
    // Forearm onto the goal
    const Vec3 elbow = bonePos(tr[c.fore]);
    rotateOnto(tr[c.fore], elbow, bonePos(tr[c.hand])-elbow, goal-elbow);
    tr[c.hand] = tr[c.fore]*localMatrix(c.hand);

    // Hand: the calibration frame (hand bone space) mapped onto the tracked frame (world)
    Matrix4x4& m = tr[c.hand];
    auto handToWorld = [&](const Vec3& v) { return boneAxis(m,0)*v.x + boneAxis(m,1)*v.y + boneAxis(m,2)*v.z; };
    const Vec3 t1 = Vec3::normalize(world(t.distal));
    const Vec3 t2 = Vec3::normalize(world(c.byThumb ? t.thumb : t.dorsal));
    const Vec3 e1 = Vec3::normalize(Vec3::normalize(handToWorld(c.frame[0]))*(1.f-w) + t1*w);
    Vec3       e2 = Vec3::normalize(handToWorld(c.frame[1]))*(1.f-w) + t2*w;
    e2 = Vec3::normalize(e2 - e1*Vec3::dotProduct(e2,e1));
    const Vec3 e3 = Vec3::crossProduct(e1,e2);
    float curl = 0;
    Vec3  curlAxis;
    if(e1.quadLength()>0.5f && e2.quadLength()>0.5f) {
      const float scale = boneAxis(m,0).length();
      for(int k=0; k<3; ++k) {
        const Vec3 v = (e1*component(c.frame[0],k) + e2*component(c.frame[1],k) + e3*component(c.frame[2],k))*scale;
        m.set(k,0,v.x);
        m.set(k,1,v.y);
        m.set(k,2,v.z);
        }
      // Fingers bend around the knuckle line, towards the palm: the target's back of the hand
      // expressed in its own frame, then carried over to the blended frame
      const Vec3 tx = Vec3::normalize(t2 - t1*Vec3::dotProduct(t2,t1));
      const Vec3 tz = Vec3::crossProduct(t1,tx);
      const Vec3 td = Vec3::normalize(world(t.dorsal));
      const Vec3 dorsal = e1*Vec3::dotProduct(td,t1) + e2*Vec3::dotProduct(td,tx) + e3*Vec3::dotProduct(td,tz);
      curlAxis = Vec3::normalize(Vec3::crossProduct(e1,-dorsal));
      if(curlAxis.quadLength()>0.5f)
        curl = std::clamp(t.curl,0.f,1.f)*w; // radians per finger joint: up to ~57 degrees
      }

    updateArmChildren(c.upper, c.fore, curlAxis, 0.f);
    updateArmChildren(c.fore,  c.hand, curlAxis, 0.f);
    updateArmChildren(c.hand,  size_t(-1), curlAxis, curl);
    }
  }

void Pose::updateArmChildren(size_t parent, size_t skip, const Vec3& curlAxis, float curl) {
  const auto& nodes = skeleton->nodes;
  for(size_t i=0; i<nodes.size(); ++i) {
    if(nodes[i].parent!=parent || i==skip)
      continue;
    tr[i] = tr[parent]*localMatrix(i);
    if(curl!=0.f && armIk.finger[i]!=0)
      rotateAround(tr[i], bonePos(tr[i]), curlAxis, curl);
    updateArmChildren(i, size_t(-1), curlAxis, curl);
    }
  }

const Animation::Sequence* Pose::solveNext(const AnimationSolver &solver, const Layer& lay) {
  auto sq = lay.seq;

  if((lay.bs & BS_ITEMINTERACT)==BS_ITEMINTERACT && itemUseSt!=itemUseDestSt) {
    int sA = itemUseSt, sB = itemUseSt, nextState = 0;
    if(itemUseSt<itemUseDestSt) {
      sB++;
      nextState = itemUseSt+1;
      } else {
      sB--;
      nextState = itemUseSt-1;
      }
    char scheme[64]={};
    sq->schemeName(scheme);

    const Animation::Sequence* ret = nullptr;
    if(itemUseSt>itemUseDestSt) {
      string_frm T_ID_SX_2_STAND("T_",scheme,"_S",itemUseSt,"_2_STAND");
      ret = solver.solveFrm(T_ID_SX_2_STAND);
      }

    if(ret==nullptr) {
      string_frm T_ID_Sa_2_Sb("T_",scheme,"_S",sA,"_2_S",sB);
      ret = solver.solveFrm(T_ID_Sa_2_Sb);
      }

    if(ret==nullptr && itemUseDestSt>=0)
      return sq;
    itemUseSt = nextState;
    return ret;
    }

  if(sq==rotation)
    return sq;

  return solver.solveNext(*sq);
  }

void Pose::addLayer(const Animation::Sequence *seq, BodyState bs, uint8_t comb, uint64_t tickCount) {
  if(seq==nullptr)
    return;
  Layer l;
  l.seq   = seq;
  l.sAnim = tickCount;
  l.bs    = bs;
  l.comb  = comb;
  lay.push_back(l);
  onAddLayer(lay.back());
  std::sort(lay.begin(),lay.end(),[](const Layer& a,const Layer& b){
    return a.seq->layer<b.seq->layer;
    });
  }

void Pose::onAddLayer(const Pose::Layer& l) {
  if(hasLayerEvents(l))
    hasEvents++;
  if(l.seq->isFly())
    isFlyCombined++;
  if(l.seq->animCls==Animation::Transition)
    hasTransitions++;
  needToUpdate = true;
  }

void Pose::onRemoveLayer(const Pose::Layer &l) {
#if defined(OPENGOTHIC_XR)
  if(flag==NoTranslation)
    needToUpdate = true;
#endif
  if(l.seq==rotation)
    rotation=nullptr;
  if(hasLayerEvents(l))
    hasEvents--;
  if(l.seq->animCls==Animation::Transition)
    hasTransitions--;
  if(l.seq->isFly())
    isFlyCombined--;

  for(auto id:l.seq->data->nodeIndex)
    if(hasSamples[id]==S_Valid)
      hasSamples[id] = S_Old;

  for(auto& lx : lay) {
    if(&lx==&l)
      break;
    lx.sBlend = lastUpdate-lx.sAnim;
    }
  }

bool Pose::hasLayerEvents(const Pose::Layer& l) {
  if(l.seq==nullptr)
    return false;
  return
      l.seq->data->events.size()>0 ||
      l.seq->data->mmStartAni.size()>0 ||
      l.seq->data->gfx.size()>0;
  }

void Pose::processSfx(Npc &npc, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processSfx(lastUpdate,i.sAnim,tickCount,&npc,nullptr);
  }

void Pose::processSfx(Interactive& mob, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processSfx(lastUpdate,i.sAnim,tickCount,nullptr,&mob);
  }

void Pose::processPfx(MdlVisual& visual, World& world, uint64_t tickCount) {
  for(auto& i:lay)
    i.seq->processPfx(lastUpdate,i.sAnim,tickCount,visual,world);
  }

bool Pose::processEvents(uint64_t &barrier, uint64_t now, Animation::EvCount &ev) const {
  if(hasEvents==0) {
    barrier=now;
    return false;
    }

  for(auto& i:lay)
    i.seq->processEvents(barrier,i.sAnim,now,ev);
  barrier=now;
  return hasEvents>0;
  }

void Pose::setObjectMatrix(const Tempest::Matrix4x4& obj, bool sync) {
  if(pos==obj)
    return;
  pos = obj;
  if(sync)
    mkSkeleton(pos); else
    needToUpdate = true;
  }

Tempest::Vec3 Pose::animMoveSpeed(uint64_t tickCount, uint64_t dt) const {
  for(size_t i=lay.size(); i>0; ) {
    --i;
    auto& lx = lay[i];
    if(!lx.seq->data->hasMoveTr)
      continue;
    return lx.seq->speed(tickCount-lx.sAnim,dt);
    }
  return Tempest::Vec3();
  }

bool Pose::isDefParWindow(uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->isDefParWindow(tickCount-i.sAnim))
      return true;
  return false;
  }

bool Pose::isDefWindow(uint64_t tickCount) const {
  for(auto& i:lay)
    if(i.seq->isDefWindow(tickCount-i.sAnim))
      return true;
  return false;
  }

bool Pose::isDefence(uint64_t tickCount) const {
  for(auto& i:lay) {
    if(i.bs==BS_PARADE && i.seq->isDefWindow(tickCount-i.sAnim))
      return true;
    }
  return false;
  }

bool Pose::isJumpBack(uint64_t tickCount) const {
  for(auto& i:lay) {
    if(i.bs==BS_PARADE && i.seq->data->defWindow.empty())
      return true;
    if(i.bs==BS_PARADE && i.seq->isDefWindow(tickCount-i.sAnim))
      return true;
    }
  return false;
  }

bool Pose::isJumpAnim() const {
  for(auto& i:lay) {
    if(i.bs!=BS_JUMP || i.seq->animCls!=Animation::Transition)
      continue;
    return true;
    }
  return false;
  }

bool Pose::isFlyAnim() const {
  return isFlyCombined>0;
  }

bool Pose::isStanding() const {
  if(lay.size()!=1 || lay[0].seq->animCls==Animation::Transition)
    return false;
  auto& s = *lay[0].seq;
  if(s.isIdle())
    return true;
  // check common idle animations
  return s.name=="S_FISTRUN"  || s.name=="S_MAGRUN"  ||
         s.name=="S_1HRUN"    || s.name=="S_BOWRUN"  ||
         s.name=="S_2HRUN"    || s.name=="S_CBOWRUN" ||
         s.name=="S_RUN"      || s.name=="S_WALK"    ||
         s.name=="S_FISTWALK" || s.name=="S_MAGWALK" ||
         s.name=="S_1HWALK"   || s.name=="S_BOWWALK" ||
         s.name=="S_2HWALK"   || s.name=="S_CBOWWALK";
  }

bool Pose::isPrehit(uint64_t now) const {
  for(auto& i:lay)
    if(i.seq->isPrehit(i.sAnim,now))
      return true;
  return false;
  }

bool Pose::isAttackAnim() const {
  for(auto& i:lay)
    if(i.seq->isAttackAnim())
      return true;
  return false;
  }

bool Pose::isIdle() const {
  for(auto& i:lay)
    if(!i.seq->isIdle())
      return false;
  return true;
  }

bool Pose::isInAnim(std::string_view sq) const {
  for(auto& i:lay)
    if(i.seq->name==sq)
      return true;
  return false;
  }

bool Pose::isInAnim(const Animation::Sequence *sq) const {
  for(auto& i:lay)
    if(i.seq==sq)
      return true;
  return false;
  }

bool Pose::hasAnim() const {
  return lay.size()>0;
  }

uint64_t Pose::animationTotalTime() const {
  uint64_t ret=0;
  for(auto& i:lay)
    ret = std::max(ret,uint64_t(i.seq->totalTime()));
  return ret;
  }

uint64_t Pose::atkTotalTime() const {
  uint16_t comboLen = combo.len();
  uint64_t ret=0;
  for(auto& i:lay)
    ret = std::max(ret,uint64_t(i.seq->atkTotalTime(comboLen)));
  return ret;
  }

const Animation::Sequence* Pose::continueCombo(const AnimationSolver &solver, const Animation::Sequence *sq,
                                               BodyState bs, uint64_t tickCount) {
  if(sq==nullptr)
    return nullptr;

  Layer* prev = nullptr;
  for(auto& i:lay)
    if(i.seq->layer==sq->layer) {
      prev = &i;
      break;
      }

  if(prev==nullptr) {
    combo = ComboState();
    return nullptr;
    }

  auto&    d     = *prev->seq->data;
  uint64_t t     = tickCount-prev->sAnim;
  size_t   id    = combo.len()*2;

  if(0==d.defWindow.size() || 0==d.defHitEnd.size()) {
    if(!startAnim(solver,sq,prev->comb,bs,Pose::NoHint,tickCount))
      return nullptr;
    combo = ComboState();
    return sq;
    }

  if(sq->data->defHitEnd.size()==0) {
    // hit -> block
    startAnim(solver,sq,prev->comb,bs,Pose::Force,tickCount);
    combo = ComboState();
    return sq;
    }

  if(id+1>=d.defWindow.size()) {
    combo.setBreak();
    return nullptr;
    }

  if(!(d.defWindow[id+0]<t && t<=d.defWindow[id+1])) {
    if(prev->seq->name==sq->name && sq->data->defHitEnd.size()>0)
      combo.setBreak();
    return nullptr;
    }

  if(combo.isBreak())
    return nullptr;

  if(prev->seq->name!=sq->name) {
    startAnim(solver,sq,prev->comb,bs,Pose::Force,tickCount);
    combo = ComboState();
    return sq;
    }

  if(combo.len()<d.defHitEnd.size())
    prev->sAnim = tickCount - d.defHitEnd[combo.len()];
  combo.incLen();
  return prev->seq;
  }

uint16_t Pose::comboLength() const {
  return combo.len();
  }

const Tempest::Matrix4x4 Pose::rootNode() const {
  size_t id = 0;
  if(skeleton->rootNodes.size())
    id = skeleton->rootNodes[0];

  const auto& nodes = skeleton->nodes;
  if(id>=nodes.size())
    return Matrix4x4::mkIdentity();

  auto ret = hasSamples[id] ? mkMatrix(base[id]) : nodes[id].tr;
  return ret;
  }

const Matrix4x4 Pose::rootBone() const {
  size_t id = 0;
  if(skeleton->rootNodes.size())
    id = skeleton->rootNodes[0];
  return tr[id];
  }

const Tempest::Matrix4x4& Pose::bone(size_t id) const {
  return tr[id];
  }

size_t Pose::boneCount() const {
  return numBones;
  }

size_t Pose::findNode(std::string_view b) const {
  if(skeleton!=nullptr)
    return skeleton->findNode(b);
  return size_t(-1);
  }

void Pose::setHeadRotation(float dx, float dz) {
#if defined(OPENGOTHIC_XR)
  if(flag==NoTranslation && (dx!=headRotX || dz!=headRotY))
    needToUpdate = true;
#endif
  headRotX = dx;
  headRotY = dz;
  }

Vec2 Pose::headRotation() const {
  return Vec2(headRotX,headRotY);
  }

void Pose::setArmTargets(const ArmTarget (&target)[2], uint64_t dt) {
  const float step = float(dt)/200.f;
  for(size_t i=0; i<2; ++i) {
    if(target[i].valid)
      armIk.target[i] = target[i];
    const float w = std::clamp(armIk.weight[i]+(target[i].valid ? step : -step), 0.f, 1.f);
    armIk.settle   |= (w==0.f && armIk.weight[i]>0.f);
    armIk.weight[i] = w;
    }
  }

void Pose::clearArmTargets() {
  armIk.settle   |= hasArmTargets();
  armIk.weight[0] = 0;
  armIk.weight[1] = 0;
  }

bool Pose::hasArmTargets() const {
  return armIk.weight[0]>0.f || armIk.weight[1]>0.f;
  }

void Pose::setAnimRotate(const AnimationSolver &solver, Npc &npc, WeaponState fightMode, AnimationSolver::TurnType turn, int dir) {
  const Animation::Sequence *sq = nullptr;
  if(dir==0) {
    if(rotation!=nullptr) {
      if(stopAnim(rotation->name))
        rotation = nullptr;
      }
    return;
    }
  if(bodyState()!=BS_STAND)
    return;

  enum AnimationSolver::Anim ani;
  if(turn==AnimationSolver::TurnType::Whirl) {
    ani = (dir<0)?AnimationSolver::Anim::WhirlL:AnimationSolver::Anim::WhirlR;
    } else {
    ani = (dir<0)?AnimationSolver::Anim::RotL:AnimationSolver::Anim::RotR;
    }

  sq = solver.solveAnim(ani,fightMode,npc.walkMode(),*this);
  if(rotation!=nullptr) {
    if(sq!=nullptr && rotation->name==sq->name)
      return;
    if(!stopAnim(rotation->name))
      return;
    }
  if(sq==nullptr)
    return;
  if(startAnim(solver,sq,0,BS_FLAG_FREEHANDS,Pose::NoHint,npc.world().tickCount())) {
    rotation = sq;
    return;
    }
  }

const Animation::Sequence* Pose::setAnimItem(const AnimationSolver &solver, Npc &npc, std::string_view scheme, int state) {
  string_frm T_ID_STAND_2_S0("T_",scheme,"_STAND_2_S0");
  const Animation::Sequence *sq = solver.solveFrm(T_ID_STAND_2_S0);
  if(startAnim(solver,sq,0,BS_ITEMINTERACT,Pose::NoHint,npc.world().tickCount())) {
    itemUseSt     = 0;
    itemUseDestSt = state;
    return sq;
    }
  return nullptr;
  }

bool Pose::stopItemStateAnim(const AnimationSolver& solver, uint64_t tickCount) {
  if(itemUseSt<0)
    return true;
  itemUseDestSt = -1;
  for(auto& i:lay)
    if(i.bs==BS_ITEMINTERACT) {
      auto next = solveNext(solver,i);
      if(next==nullptr)
        continue;
      onRemoveLayer(i);
      i.seq   = next;
      i.sAnim = tickCount;
      onAddLayer(i);
      }
  return true;
  }

const Matrix4x4* Pose::transform() const {
  return tr;
  }

Vec3 Pose::mkBaseTranslation() {
  if(numBones==0)
    return Vec3();

  auto  b0 = rootNode();

  float dx = b0.at(3,0);
  float dy = 0;
  float dz = b0.at(3,2);

  if((flag&NoTranslation)==NoTranslation) {
    dy = b0.at(3,1);
    // dy = b0.at(3,1) - translateY();
    }

  return Vec3(-dx,-dy,-dz);
  }

template<class T, class F>
void Pose::removeIf(T &t, F f) {
  size_t ret=0;
  for(size_t i=0;i<t.size();++i) {
    if( !f(t[i]) ) {
      if(ret!=i)
        t[ret] = t[i];
      ret++;
      }
    }
  t.resize(ret);
  }
