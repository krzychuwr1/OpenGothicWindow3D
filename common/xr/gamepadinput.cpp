#include "gamepadinput.h"

#include <algorithm>
#include <bitset>
#include <cmath>

using namespace Tempest;

static float pick(float a, float b) {
  return std::abs(a)>=std::abs(b) ? a : b;
  }

PadState PadState::merge(const PadState& a, const PadState& b) {
  PadState r;
  r.connected = a.connected || b.connected;
  r.lx = pick(a.lx,b.lx);
  r.ly = pick(a.ly,b.ly);
  r.rx = pick(a.rx,b.rx);
  r.ry = pick(a.ry,b.ry);
  r.lt = std::max(a.lt,b.lt);
  r.rt = std::max(a.rt,b.rt);
  for(int i=0; i<Count; ++i)
    r.buttons[i] = a.buttons[i] || b.buttons[i];
  return r;
  }

GamepadInput::GamepadInput(KeySink sink)
  :sink(std::move(sink)) {
  }

Event::KeyType GamepadInput::keyOf(Out o) {
  // Default Gothic II keys (KeyCodec::setDefaultKeysG1 and the [KEYS] section of Gothic.ini)
  switch(o) {
    case O_Forward:   return Event::K_Up;
    case O_Back:      return Event::K_Down;
    case O_StrafeL:   return Event::K_Delete;
    case O_StrafeR:   return Event::K_PageDown;
    case O_Action:    return Event::K_LControl;
    case O_Walk:      return Event::K_LShift;
    case O_Jump:      return Event::K_LAlt;
    case O_Confirm:   return Event::K_Return;
    case O_Weapon:    return Event::K_Space;
    case O_Cancel:    return Event::K_ESCAPE;
    case O_Inventory: return Event::K_Tab;
    case O_Sneak:     return Event::K_A;
    case O_Menu:      return Event::K_ESCAPE;
    case O_Log:       return Event::K_L;
    case O_Status:    return Event::K_B;
    case O_Map:       return Event::K_M;
    case O_Up:        return Event::K_Up;
    case O_Down:      return Event::K_Down;
    case O_Left:      return Event::K_Left;
    case O_Right:     return Event::K_Right;
    case O_Count:     break;
    }
  return Event::K_NoKey;
  }

void GamepadInput::set(Out o, bool down, bool cancelRelease) {
  if(cancelled[o]) {
    cancelled[o] = down; // Wait for release before treating this as a new press.
    return;
    }
  if(state[o]==down)
    return;
  state[o] = down;
  // Start and menu Back share Escape. The key stays down until both release.
  for(int i=0; i<O_Count; ++i)
    if(i!=o && state[i] && keyOf(Out(i))==keyOf(o))
      return;
  const auto type = cancelRelease ? Event::KeyCancel : (down ? Event::KeyDown : Event::KeyUp);
  KeyEvent e(keyOf(o), Event::M_NoModifier, type);
  sink(e,down);
  }

template<class Emit>
void GamepadInput::map(const PadState& p, bool menuContext, Emit emit) const {
  // Hysteresis keeps a stick resting near the threshold from chattering
  auto neg = [this](Out o, float v) { return state[o] ? v<-0.35f : v<-0.55f; };
  auto pos = [this](Out o, float v) { return state[o] ? v> 0.35f : v> 0.55f; };
  const bool* b = p.buttons;

  if(menuContext) {
    // Menus, dialogs and documents navigate with arrow keys
    for(Out o:{O_Forward,O_Back,O_StrafeL,O_StrafeR,O_Action,O_Walk,O_Jump,O_Weapon,O_Sneak})
      emit(o,false);
    emit(O_Up,      neg(O_Up,p.ly)    || b[PadState::DpadUp]);
    emit(O_Down,    pos(O_Down,p.ly)  || b[PadState::DpadDown]);
    emit(O_Left,    neg(O_Left,p.lx)  || b[PadState::DpadLeft]);
    emit(O_Right,   pos(O_Right,p.lx) || b[PadState::DpadRight]);
    emit(O_Confirm, b[PadState::A] || p.rt>0.6f || b[PadState::RightShoulder]);
    emit(O_Cancel,  b[PadState::B]);
    } else {
    for(Out o:{O_Up,O_Down,O_Left,O_Right,O_Confirm,O_Cancel})
      emit(o,false);
    emit(O_Forward, neg(O_Forward,p.ly) || b[PadState::DpadUp]);
    emit(O_Back,    pos(O_Back,p.ly)    || b[PadState::DpadDown]);
    emit(O_StrafeL, neg(O_StrafeL,p.lx) || b[PadState::DpadLeft]);
    emit(O_StrafeR, pos(O_StrafeR,p.lx) || b[PadState::DpadRight]);
    emit(O_Action,  p.rt>0.6f || b[PadState::RightShoulder]);
    emit(O_Walk,    p.lt>0.6f || b[PadState::LeftShoulder]);
    emit(O_Jump,    b[PadState::A]);
    emit(O_Weapon,  b[PadState::B]);
    emit(O_Sneak,   b[PadState::Y]);
    }
  emit(O_Inventory, p.buttons[PadState::X]);
  emit(O_Menu,      p.buttons[PadState::Start]);
  emit(O_Log,       p.buttons[PadState::Back]);
  emit(O_Status,    p.buttons[PadState::LeftStick]);
  emit(O_Map,       p.buttons[PadState::RightStick]);
  }

void GamepadInput::tick(const PadState& p, bool menuContext) {
  map(p,menuContext,[this](Out o,bool down){ set(o,down); });
  }

void GamepadInput::cancelMissing(const PadState& previous, bool previousMenu, const PadState& current, bool currentMenu) {
  std::bitset<Event::K_Last> retained;
  auto keep = [&](Out o,bool down) {
    if(down)
      retained.set(keyOf(o));
    };
  map(previous,previousMenu,keep);
  map(current,currentMenu,[&](Out o,bool down) {
    if(!down || cancelled[o])
      return;
    keep(o,true);
    // Transfer an existing capture before tick releases its old alias.
    if(!state[o]) {
      for(int i=0; i<O_Count; ++i)
        if(state[i] && keyOf(Out(i))==keyOf(o)) {
          state[o] = true;
          break;
          }
      }
    });
  for(int i=0; i<O_Count; ++i) {
    const auto o = Out(i);
    if(state[o] && !retained[keyOf(o)])
      set(o,false,true);
    }
  }

PointF GamepadInput::look(const PadState& p, uint64_t dtMs) const {
  auto shape = [](float v) {
    const float dead = 0.15f;
    const float a    = std::abs(v);
    if(a<dead)
      return 0.f;
    const float t = (a-dead)/(1.f-dead);
    return std::copysign(t*t, v);
    };
  // Full deflection turns about 180 degrees per second with the default mouse sensitivity
  const float speed = 1400.f*float(dtMs)/1000.f;
  return PointF(shape(p.rx)*speed, shape(p.ry)*speed*0.6f);
  }

void GamepadInput::releaseAll() {
  for(int i=0; i<O_Count; ++i)
    if(state[i])
      set(Out(i),false);
  }

void GamepadInput::cancel() {
  for(int i=0; i<O_Count; ++i) {
    cancelled[i] |= state[i];
    state[i] = false;
    }
  }
