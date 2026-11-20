#pragma once

#include <array>
#include <future>

#include "camera.h"
#include "resources.h"

#include <Tempest/Window>
#include <Tempest/CommandBuffer>
#include <Tempest/Fence>
#include <Tempest/VulkanApi>
#include <Tempest/Device>
#include <Tempest/VertexBuffer>
#include <Tempest/UniformBuffer>
#include <Tempest/VectorImage>
#include <Tempest/Event>
#include <Tempest/Pixmap>
#include <Tempest/Sprite>
#include <Tempest/Font>
#include <Tempest/TextureAtlas>
#include <Tempest/Timer>
#include <Tempest/Swapchain>

#include <vector>
#include <thread>
#include <optional>

#include "world/world.h"
#include "world/focus.h"
#include "game/playercontrol.h"
#include "graphics/renderer.h"
#include "ui/dialogmenu.h"
#include "ui/inventorymenu.h"
#include "xr/gamepadinput.h"
#include "ui/chapterscreen.h"
#include "ui/documentmenu.h"
#include "ui/videowidget.h"
#include "ui/menuroot.h"
#include "ui/consolewidget.h"
#include "ui/touchinput.h"

#include "utils/keycodec.h"
#include "resources.h"

class MenuRoot;
class GameSession;
class Interactive;

class MainWindow : public Tempest::Window {
  public:
    explicit MainWindow(Tempest::Device& device);
    ~MainWindow() override;

    float uiScale() const;

  private:
    void paintEvent     (Tempest::PaintEvent& event) override;
    void resizeEvent    (Tempest::SizeEvent & event) override;
    void metricsEvent   (Tempest::DisplayMetricsEvent& event) override;

    void mouseDownEvent (Tempest::MouseEvent& event) override;
    void mouseUpEvent   (Tempest::MouseEvent& event) override;
    void mouseDragEvent (Tempest::MouseEvent& event) override;
    void mouseMoveEvent (Tempest::MouseEvent& event) override;
    void mouseWheelEvent(Tempest::MouseEvent& event) override;

    void keyDownEvent   (Tempest::KeyEvent&   event) override;
    void keyRepeatEvent (Tempest::KeyEvent&   event) override;
    void keyUpEvent     (Tempest::KeyEvent&   event) override;

#if defined(__ANDROID__)
    void keyCancelEvent (Tempest::KeyEvent& event) override;
#endif

    void focusEvent     (Tempest::FocusEvent&  event) override;

    void paintFocus     (Tempest::Painter& p, const Focus& fc, const Tempest::Matrix4x4& vp);
    void paintFocus     (Tempest::Painter& p, Tempest::Rect rect);
    void paintFocusMarkers(Tempest::Painter& p, const Focus& fc, const Tempest::Matrix4x4& vp);
    void paintEyeOverlay(const Camera& camera);

    void drawBar(Tempest::Painter& p, const Tempest::Texture2d *bar, int x, int y, float v, Tempest::AlignFlag flg);
    void drawMsg(Tempest::Painter& p);
    void drawProgress(Tempest::Painter& p, int x, int y, int w, int h, float v);
    void drawLoading (Tempest::Painter& p,int x,int y,int w,int h);
    void drawSaving  (Tempest::Painter& p);
    void drawSaving  (Tempest::Painter& p, const Tempest::Texture2d& back, int w, int h, float scale);

    void startGame(std::string_view slot);
    void loadGame (std::string_view slot);
    void saveGame (std::string_view slot, std::string_view name);

    void onVideo(std::string_view fname);
    void onStartLoading();
    void onWorldLoaded();
    void onSessionExit();
    void onBenchmarkFinished();
    void setGameImpl(std::unique_ptr<GameSession>&& w);
    void clearInput();
    void setFullscreen(bool fs);

    void processMouse(Tempest::MouseEvent& event, bool enable);
    void tickMouse(uint64_t dt);
    void onSettings();

    void setupUi();

    void render() override;

    uint64_t tick();
    void     tickPad(uint64_t dt);
    bool     padMenuContext() const;
    void     renderFrame(bool xr);
    void     updateFrame();
    void     runStartupCommands();

    struct FrameTiming {
      uint64_t frames = 0, total = 0, wait = 0, update = 0, gpuWait = 0, record = 0, present = 0;
      uint64_t uiMesh = 0, encStart = 0, drawCall = 0, submit = 0;
      uint64_t tickGame = 0, tickAnim = 0, tickCam = 0;
      uint64_t last = 0;
      void     report();
      };
    FrameTiming timing;
    bool        startupDone   = false;
    int         startupFrames = 0;
    uint32_t outW() const;
    uint32_t outH() const;
    void     updateAnimation(uint64_t dt);
    void     tickCamera(uint64_t dt);
    void     isDialogClosed(bool& ret);

    template<Tempest::KeyEvent::KeyType k>
    void     onMarvinKey();

    Camera::Mode solveCameraMode() const;

    enum RuntimeMode : uint8_t {
      R_Normal,
      R_Suspended,
      R_Step,
      };

    Tempest::Device&      device;
    std::optional<Tempest::Swapchain> swapchain;
    Tempest::Size         xrSize;
    Tempest::Matrix4x4    prevEyeViewProject[2];
    bool                  eyeHistoryValid = false;
    GamepadInput          pad;
#if defined(__ANDROID__)
    bool                  previousPadMenu = false;
#endif
    Tempest::PointF       padLook;
    uint64_t              padTime = 0;
    Tempest::TextureAtlas atlas;
    Shaders               shaders;
    Renderer              renderer;

    Tempest::VectorImage  uiLayer, numOverlay;
    Tempest::VectorImage::Mesh uiMesh [Resources::MaxFramesInFlight];
    Tempest::VectorImage::Mesh numMesh[Resources::MaxFramesInFlight];
    std::future<void>          captureWriter[2];
    uint64_t                   captureNext = 0;
    uint32_t                   captureCount = 0;
    bool                       captureDue = false;
    Tempest::VectorImage       eyeOverlay; // XR: interface drawn per eye at scene depth
    Tempest::VectorImage::Mesh eyeMesh[Resources::MaxFramesInFlight];

    Tempest::Fence         fence   [Resources::MaxFramesInFlight];
    Tempest::CommandBuffer commands[Resources::MaxFramesInFlight];
    uint8_t                cmdId = 0;

    Tempest::Texture2d        background;
    const Tempest::Texture2d* loadBox=nullptr;
    const Tempest::Texture2d* loadVal=nullptr;

    const Tempest::Texture2d* barBack=nullptr;
    const Tempest::Texture2d* barHp  =nullptr;
    const Tempest::Texture2d* barMisc=nullptr;
    const Tempest::Texture2d* barMana=nullptr;

    const Tempest::Texture2d* focusImg=nullptr;

    const Tempest::Texture2d* saveback=nullptr;

    bool                      mouseP[Tempest::MouseEvent::ButtonBack]={};

    KeyCodec                  keycodec;

    MenuRoot                  rootMenu;
    VideoWidget               video;
    InventoryMenu             inventory;
    DialogMenu                dialogs;
    DocumentMenu              document;
    ChapterScreen             chapter;
    ConsoleWidget             console;
#if defined(__MOBILE_PLATFORM__)
    TouchInput                mobileUi;
#endif
    RuntimeMode               runtimeMode = R_Normal;

    Tempest::Widget*&         keyRecipient(Tempest::Event::KeyType key);
#if defined(__ANDROID__)
    std::array<Tempest::Widget*,Tempest::Event::K_Last> keyRecipients = {};
#else
    Tempest::Widget*          lastKeyRecipient=nullptr;
#endif
    Tempest::Point            dMouse;
    PlayerControl             player;
    uint64_t                  lastTick=0;

    Tempest::Shortcut         funcKey[11];
    Tempest::Shortcut         displayPos;

    struct BenchmarkData {
      std::vector<uint64_t> low1procent;
      size_t                numFrames = 0;
      double                fpsSum = 0;
      void                  push(uint64_t t);
      void                  clear();
      };
    struct Fps {
      uint64_t dt[10]={};
      double   get() const;
      void     push(uint64_t t);
      };
    Fps           fps;
    BenchmarkData benchmark;
    uint64_t      maxFpsInv = 0;
  };
