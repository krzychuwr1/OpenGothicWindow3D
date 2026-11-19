#pragma once

#include <Tempest/Event>
#include <Tempest/Point>

#include <cstdint>
#include <functional>

// One controller layout shared by Bluetooth gamepads and XR controllers.
// The pad emits the game's default keyboard keys, so menus, dialogs and
// the player controller handle it exactly like a keyboard.
struct PadState {
  enum Button : uint8_t {
    A, B, X, Y, LeftShoulder, RightShoulder, LeftStick, RightStick, Start, Back,
    DpadUp, DpadDown, DpadLeft, DpadRight, Count
    };
  bool  connected = false;
  float lx = 0, ly = 0;   // left stick, y down is positive (Android convention)
  float rx = 0, ry = 0;   // right stick
  float lt = 0, rt = 0;   // triggers 0..1
  bool  buttons[Count] = {};

  // Combines two sources: the larger deflection and any pressed button win.
  static PadState merge(const PadState& a, const PadState& b);
  };

class GamepadInput final {
  public:
    using KeySink = std::function<void(Tempest::KeyEvent& e, bool down)>;

    explicit GamepadInput(KeySink sink);

    // Context changes what A and B do: confirm/back in menus, jump/weapon in the world.
    void tick(const PadState& pad, bool menuContext);
    // Camera rotation from the right stick, in mouse pixels for this frame.
    Tempest::PointF look(const PadState& pad, uint64_t dtMs) const;

    void releaseAll();

  private:
    enum Out : uint8_t {
      O_Forward, O_Back, O_StrafeL, O_StrafeR,
      O_Action, O_Walk, O_Jump, O_Confirm, O_Weapon, O_Cancel, O_Inventory, O_Sneak,
      O_Menu, O_Log, O_Status, O_Map,
      O_Up, O_Down, O_Left, O_Right,
      O_Count
      };

    void set(Out o, bool down);

    static Tempest::Event::KeyType keyOf(Out o);

    KeySink sink;
    bool    state[O_Count] = {};
  };
