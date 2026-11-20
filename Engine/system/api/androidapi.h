#pragma once

#include <Tempest/SystemApi>
#include <filesystem>

struct ANativeWindow;
struct ANativeActivity;

namespace Tempest {

class AndroidApi final : SystemApi {
  public:
    struct GamepadState {
      enum Button : uint8_t {
        A, B, X, Y, LeftShoulder, RightShoulder, LeftStick, RightStick, Start, Back,
        DpadUp, DpadDown, DpadLeft, DpadRight, Count
        };
      bool  connected = false;
      float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
      bool  buttons[Count] = {};
      };

    struct GamepadSample {
      GamepadState state;
      GamepadState releaseSources;
      bool         interrupted = false;
      };

    static std::filesystem::path internalDataPath();
    static std::filesystem::path externalDataPath();
    // Current window of the activity, or null while it is hidden or for another activity
    static ANativeWindow*        nativeWindow(SystemApi::Window* w);
    static ANativeActivity*      activity();

    // Presentation owned by the application (for example an OpenXR session) instead of the activity's window:
    // the main window reports this client size and UI scale, and renders whether or not Android shows a window.
    static void                  setExternalPresentation(int width, int height, float uiScale);
    static bool                  hasExternalPresentation();
    static bool                  activityResumed();
    static void                  setExternalInputFocus(bool hasFocus);

    // Latest state of a Bluetooth/USB gamepad; its buttons never reach the window as key events.
    static GamepadState          gamepad();
    static GamepadSample         pollGamepad();
    // Deliver a mapped gamepad key to the focused widget, including modal overlays.
    static void                  dispatchGamepadKey(Tempest::Window& window, KeyEvent& event);

    // Pointer capture delivers relative mouse motion.
    static void                  setPointerCapture(bool capture);

  private:
    AndroidApi() = default;

    static Window* createAndroidWindow(Tempest::Window* owner);
    static void onAppCmd(void* app, int32_t cmd);
    static int32_t onInputEvent(const void* event);
    static bool processTouchEvents(AppCallBack& cb);
    static bool processKeyEvent(const void* event);
    static void dispatchInputKey(Tempest::Window& window, KeyEvent& event, uint64_t source, bool repeat);
    static bool processMouseEvent(const void* event);
    static Rect implClientRectOf();
    static void updateFocus(bool cancel = false);
    static void updateWindow();

    Window* implCreateWindow(Tempest::Window* owner, uint32_t width, uint32_t height) override;
    Window* implCreateWindow(Tempest::Window* owner, ShowMode sm) override;
    void    implDestroyWindow(Window* w) override;
    void    implExit() override;

    Rect implWindowClientRect(SystemApi::Window* w) override;
    float implUiScale(SystemApi::Window* w) override;
    bool implSetAsFullscreen(SystemApi::Window* w, bool fullScreen) override;
    bool implIsFullscreen(SystemApi::Window* w) override;

    void implSetCursorPosition(SystemApi::Window* w, int x, int y) override;
    void implShowCursor(SystemApi::Window* w, CursorShape cursor) override;

    bool implIsRunning() override;
    int  implExec(AppCallBack& cb) override;
    void implProcessEvents(AppCallBack& cb) override;

    void implSetWindowTitle(SystemApi::Window* w, const char* utf8) override;

  friend class SystemApi;
  };

}
