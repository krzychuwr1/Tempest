#include "androidapi.h"

#ifdef __ANDROID__

#include <android_native_app_glue.h>

#include <Tempest/Event>
#include <Tempest/Log>
#include <Tempest/Window>

#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/configuration.h>
#include <android/keycodes.h>
#include <jni.h>

#include <algorithm>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <dlfcn.h>
#include <exception>
#include <thread>
#include <vector>

using namespace Tempest;

extern "C" void android_main(android_app* state);

static int32_t AMotionEvent_getPointerIndex(const AInputEvent* motion_event) {
  const int32_t action = AMotionEvent_getAction(motion_event);
  return int32_t((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  };

static Tempest::Point AMotionEvent_getXY(const AInputEvent* event, size_t i) {
  return Point(int(AMotionEvent_getX(event,i)), int(AMotionEvent_getY(event,i)));
  };

static android_app*     app        = nullptr;
static Tempest::Window* mainWindow = nullptr;
static std::atomic_bool isExit     = false;
static bool             resumed    = false;
static bool             focused    = false;
static bool             active     = false;
static bool             hasWindow  = false;

static bool             externalPresentation = false;
static bool             externalInputFocus   = true;
static int              externalW            = 0;
static int              externalH            = 0;
static float            externalUiScale      = 1.f;

static AndroidApi::GamepadState gamepadState;
static AndroidApi::GamepadState gamepadKeys;
// Android can expose separate input device IDs for buttons and axes.
static int32_t                  axisDevice = -1, keyDevice = -1;
static std::chrono::steady_clock::time_point nextPadCheck;
static Point                    mouseLast;
static bool                     mouseValid = false;
static int32_t                  mousePressed = 0;

struct HeldKey {
  Event::KeyType key;
  uint64_t       source;
  };
static std::vector<HeldKey> heldKeys;

// Touches are recorded during polling and dispatched one per event-processing pass.
// Modal loops in event handlers poll recursively, so direct dispatch could reorder pointers of one AInputEvent.
struct TouchPoint {
  int32_t id   = -1;
  bool    down = false;
  bool    up   = false;
  Point   downPos;
  Point   movePos, moveLast;
  };
static std::vector<TouchPoint> touches;

static void pressTouch(int32_t id, Point pos) {
  size_t slot = 0;
  // A released slot is reused only after its release is dispatched
  while(slot<touches.size() && (touches[slot].id!=-1 || touches[slot].up))
    ++slot;
  if(slot==touches.size())
    touches.emplace_back();
  auto& t = touches[slot];
  t         = TouchPoint();
  t.id      = id;
  t.downPos = pos;
  t.movePos = pos;
  t.down    = true;
  }

static void updateTouch(int32_t id, Point pos, bool release) {
  for(auto& t:touches) {
    if(t.id!=id)
      continue;
    t.movePos = pos;
    if(release) {
      t.id = -1;
      t.up = true;
      }
    return;
    }
  }

static void releaseAllTouches() {
  for(auto& t:touches) {
    if(t.id==-1)
      continue;
    t.id = -1;
    t.up = true;
    }
  }

static Event::KeyType androidKey(int32_t code) {
  if(code>=AKEYCODE_A && code<=AKEYCODE_Z)
    return Event::KeyType(Event::K_A + (code-AKEYCODE_A));
  if(code>=AKEYCODE_0 && code<=AKEYCODE_9)
    return Event::KeyType(Event::K_0 + (code-AKEYCODE_0));
  if(code>=AKEYCODE_F1 && code<=AKEYCODE_F12)
    return Event::KeyType(Event::K_F1 + (code-AKEYCODE_F1));
  switch(code) {
    case AKEYCODE_ESCAPE:        return Event::K_ESCAPE;
    case AKEYCODE_CTRL_LEFT:     return Event::K_LControl;
    case AKEYCODE_CTRL_RIGHT:    return Event::K_RControl;
    case AKEYCODE_SHIFT_LEFT:    return Event::K_LShift;
    case AKEYCODE_SHIFT_RIGHT:   return Event::K_RShift;
    case AKEYCODE_ALT_LEFT:      return Event::K_LAlt;
    case AKEYCODE_ALT_RIGHT:     return Event::K_RAlt;
    case AKEYCODE_META_LEFT:     return Event::K_LCommand;
    case AKEYCODE_META_RIGHT:    return Event::K_RCommand;
    case AKEYCODE_INSERT:        return Event::K_Insert;
    case AKEYCODE_FORWARD_DEL:   return Event::K_Delete;
    case AKEYCODE_MOVE_HOME:     return Event::K_Home;
    case AKEYCODE_MOVE_END:      return Event::K_End;
    case AKEYCODE_PAGE_UP:       return Event::K_PageUp;
    case AKEYCODE_PAGE_DOWN:     return Event::K_PageDown;
    case AKEYCODE_DPAD_LEFT:     return Event::K_Left;
    case AKEYCODE_DPAD_UP:       return Event::K_Up;
    case AKEYCODE_DPAD_RIGHT:    return Event::K_Right;
    case AKEYCODE_DPAD_DOWN:     return Event::K_Down;
    case AKEYCODE_DEL:           return Event::K_Back;
    case AKEYCODE_TAB:           return Event::K_Tab;
    case AKEYCODE_ENTER:         return Event::K_Return;
    case AKEYCODE_NUMPAD_ENTER:  return Event::K_Return;
    case AKEYCODE_BREAK:         return Event::K_Pause;
    case AKEYCODE_SPACE:         return Event::K_Space;
    case AKEYCODE_CAPS_LOCK:     return Event::K_CapsLock;
    }
  return Event::K_NoKey;
  }

static uint32_t translateChar(int32_t code, int32_t meta) {
  const bool shift = (meta & AMETA_SHIFT_ON)!=0;
  if(code>=AKEYCODE_A && code<=AKEYCODE_Z) {
    const bool upper = shift != ((meta & AMETA_CAPS_LOCK_ON)!=0);
    return uint32_t((upper ? 'A' : 'a') + (code-AKEYCODE_A));
    }
  if(code>=AKEYCODE_0 && code<=AKEYCODE_9)
    return uint32_t((shift ? ")!@#$%^&*(" : "0123456789")[code-AKEYCODE_0]);
  switch(code) {
    case AKEYCODE_SPACE:  return ' ';
    case AKEYCODE_MINUS:  return shift ? '_' : '-';
    case AKEYCODE_PERIOD: return shift ? '>' : '.';
    case AKEYCODE_COMMA:  return shift ? '<' : ',';
    }
  return 0;
  }

static Event::Modifier translateModifiers(int32_t meta) {
  uint8_t m = Event::M_NoModifier;
  if(meta & AMETA_SHIFT_ON) m |= Event::M_Shift;
  if(meta & AMETA_ALT_ON)   m |= Event::M_Alt;
  if(meta & AMETA_CTRL_ON)  m |= Event::M_Ctrl;
  return Event::Modifier(m);
  }

static int gamepadButton(int32_t code, bool padSource) {
  using B = AndroidApi::GamepadState;
  switch(code) {
    case AKEYCODE_BUTTON_A:      return B::A;
    case AKEYCODE_BUTTON_B:      return B::B;
    case AKEYCODE_BUTTON_X:      return B::X;
    case AKEYCODE_BUTTON_Y:      return B::Y;
    case AKEYCODE_BUTTON_L1:     return B::LeftShoulder;
    case AKEYCODE_BUTTON_R1:     return B::RightShoulder;
    case AKEYCODE_BUTTON_THUMBL: return B::LeftStick;
    case AKEYCODE_BUTTON_THUMBR: return B::RightStick;
    case AKEYCODE_BUTTON_START:  return B::Start;
    case AKEYCODE_BUTTON_SELECT: return B::Back;
    case AKEYCODE_BUTTON_MODE:   return B::Back;
    case AKEYCODE_DPAD_UP:       return padSource ? B::DpadUp : -1;
    case AKEYCODE_DPAD_DOWN:     return padSource ? B::DpadDown : -1;
    case AKEYCODE_DPAD_LEFT:     return padSource ? B::DpadLeft : -1;
    case AKEYCODE_DPAD_RIGHT:    return padSource ? B::DpadRight : -1;
    }
  return -1;
  }

static bool isGamepadSource(int32_t source) {
  return (source & AINPUT_SOURCE_GAMEPAD)==AINPUT_SOURCE_GAMEPAD ||
         (source & AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK;
  }

std::filesystem::path AndroidApi::internalDataPath() {
  assert(app!=nullptr && app->activity!=nullptr);
  return app->activity->internalDataPath;
  }

std::filesystem::path AndroidApi::externalDataPath() {
  assert(app!=nullptr && app->activity!=nullptr);
  const char* path = app->activity->externalDataPath;
  return path!=nullptr ? path : "";
  }

ANativeActivity* AndroidApi::activity() {
  return app!=nullptr ? app->activity : nullptr;
  }

void AndroidApi::setExternalPresentation(int width, int height, float uiScale) {
  externalPresentation = width>0 && height>0;
  externalW            = width;
  externalH            = height;
  externalUiScale      = uiScale;
  if(mainWindow!=nullptr && externalPresentation) {
    SizeEvent event(externalW,externalH);
    AndroidApi::dispatchResize(*mainWindow,event);
    }
  }

bool AndroidApi::hasExternalPresentation() {
  return externalPresentation;
  }

bool AndroidApi::activityResumed() {
  return resumed;
  }

void AndroidApi::setExternalInputFocus(bool hasFocus) {
  if(externalInputFocus==hasFocus)
    return;
  externalInputFocus = hasFocus;
  updateFocus(!hasFocus);
  }

static void selectPad(int32_t& current, AndroidApi::GamepadState& state, const AInputEvent* event) {
  const int32_t id = AInputEvent_getDeviceId(event);
  if(current!=id) {
    state = {};
    current = id;
    }
  state.connected = true;
  // Input itself proves that this device is present.
  if((!gamepadState.connected || axisDevice==id) && (!gamepadKeys.connected || keyDevice==id))
    nextPadCheck = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  }

static void checkPadDevices() {
  if(!gamepadState.connected && !gamepadKeys.connected)
    return;
  const auto now = std::chrono::steady_clock::now();
  if(now<nextPadCheck)
    return;
  nextPadCheck = now + std::chrono::milliseconds(500);
  // NativeActivity receives button/axis events, but a removed device is
  // not guaranteed to deliver a final neutral sample.
  auto vm = app->activity->vm;
  JNIEnv* env = nullptr;
  const bool attach = vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)==JNI_EDETACHED;
  if(attach && vm->AttachCurrentThread(&env,nullptr)!=JNI_OK)
    return;
  if(env==nullptr)
    return;
  jclass cls = env->FindClass("android/view/InputDevice");
  jmethodID method = cls ? env->GetStaticMethodID(cls,"getDevice","(I)Landroid/view/InputDevice;") : nullptr;
  auto removed = [&](int32_t id) {
    if(method==nullptr || env->ExceptionCheck())
      return false;
    jobject obj = env->CallStaticObjectMethod(cls,method,jint(id));
    const bool gone = obj==nullptr && !env->ExceptionCheck();
    env->DeleteLocalRef(obj);
    return gone;
    };
  const bool axisGone = gamepadState.connected && removed(axisDevice);
  const bool sharedDevice = gamepadState.connected && keyDevice==axisDevice;
  if(gamepadKeys.connected && (sharedDevice ? axisGone : removed(keyDevice)))
    gamepadKeys = {};
  if(axisGone)
    gamepadState = {};
  if(env->ExceptionCheck())
    env->ExceptionClear();
  env->DeleteLocalRef(cls);
  if(attach)
    vm->DetachCurrentThread();
  }

AndroidApi::GamepadState AndroidApi::gamepad() {
  checkPadDevices();
  auto state = gamepadState;
  state.connected |= gamepadKeys.connected;
  state.lt = std::max(state.lt, gamepadKeys.lt);
  state.rt = std::max(state.rt, gamepadKeys.rt);
  for(int i=0; i<GamepadState::Count; ++i)
    state.buttons[i] |= gamepadKeys.buttons[i];
  return state;
  }

void AndroidApi::dispatchInputKey(Tempest::Window& window, KeyEvent& event, uint64_t source, bool repeat) {
  const uint32_t capture = 0x80000000u | uint32_t(event.key);
  auto sameKey = [&](const HeldKey& held) { return held.key==event.key; };
  auto it = std::find_if(heldKeys.begin(), heldKeys.end(), [&](const HeldKey& held) {
    return sameKey(held) && held.source==source;
    });
  if(event.type()==Event::KeyDown) {
    if(it==heldKeys.end()) {
      if(repeat) // The original press was cancelled on focus loss.
        return;
      const bool alreadyDown = std::any_of(heldKeys.begin(), heldKeys.end(), sameKey);
      heldKeys.push_back({event.key,source});
      if(alreadyDown)
        return;
      }
    dispatchKeyDown(window,event,capture);
    } else if(it!=heldKeys.end()) {
    heldKeys.erase(it);
    if(std::none_of(heldKeys.begin(), heldKeys.end(), sameKey))
      dispatchKeyUp(window,event,capture);
    }
  }

void AndroidApi::dispatchGamepadKey(Tempest::Window& window, KeyEvent& event) {
  // Physical scans use their device ID and a real scan or a tagged keycode.
  dispatchInputKey(window,event,UINT64_MAX,false);
  }

void AndroidApi::setPointerCapture(bool capture) {
  if(app==nullptr || app->activity==nullptr || app->activity->vm==nullptr)
    return;
  JNIEnv* env      = nullptr;
  bool    attached = false;
  if(app->activity->vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK) {
    if(app->activity->vm->AttachCurrentThread(&env,nullptr)!=JNI_OK)
      return;
    attached = true;
    }
  jobject   activityObj = app->activity->clazz;
  jclass    activityCls = env->GetObjectClass(activityObj);
  jmethodID getWindow   = env->GetMethodID(activityCls,"getWindow","()Landroid/view/Window;");
  jobject   window      = env->CallObjectMethod(activityObj,getWindow);
  if(window!=nullptr) {
    jclass    windowCls = env->GetObjectClass(window);
    jmethodID getDecor  = env->GetMethodID(windowCls,"getDecorView","()Landroid/view/View;");
    jobject   decor     = env->CallObjectMethod(window,getDecor);
    if(decor!=nullptr) {
      jclass    viewCls = env->GetObjectClass(decor);
      jmethodID method  = env->GetMethodID(viewCls,capture ? "requestPointerCapture" : "releasePointerCapture","()V");
      if(method!=nullptr)
        env->CallVoidMethod(decor,method);
      }
    }
  if(env->ExceptionCheck()) {
    env->ExceptionClear();
    Log::e("AndroidApi: pointer capture request failed");
    }
  if(attached)
    app->activity->vm->DetachCurrentThread();
  }

void AndroidApi::updateFocus(bool cancel) {
  const bool next = resumed && focused && (!externalPresentation || externalInputFocus);
  if(active==next && !cancel)
    return;
  active = next;
  if(!active) {
    heldKeys.clear();
    gamepadState = {};
    gamepadKeys = {};
    mouseValid = false;
    mousePressed = 0;
    touches.clear(); // Drop pending downs together with the dispatcher's captures.
    }
  if(mainWindow!=nullptr) {
    FocusEvent event(active,Event::FocusReason::UnknownReason);
    AndroidApi::dispatchFocus(*mainWindow,event);
    }
  }

void AndroidApi::updateWindow() {
  if(app==nullptr || app->window==nullptr)
    return;

  hasWindow = true;
  if(mainWindow==nullptr || externalPresentation)
    return;

  SizeEvent event(ANativeWindow_getWidth(app->window),ANativeWindow_getHeight(app->window));
  AndroidApi::dispatchResize(*mainWindow,event);
  }

void AndroidApi::onAppCmd(void*, int32_t cmd) {
  switch(cmd) {
    case APP_CMD_INIT_WINDOW:
      updateWindow();
      break;
    case APP_CMD_WINDOW_RESIZED:
      updateWindow();
      break;
    case APP_CMD_CONFIG_CHANGED: {
      //TODO: dedicated event for dpi/safe-rects
      if(mainWindow!=nullptr && app!=nullptr && app->window!=nullptr && !externalPresentation) {
        SizeEvent event(ANativeWindow_getWidth(app->window), ANativeWindow_getHeight(app->window));
        SystemApi::dispatchResize(*mainWindow, event);
        }
      break;
      }
    case APP_CMD_GAINED_FOCUS:
      focused = true;
      updateFocus();
      break;
    case APP_CMD_LOST_FOCUS:
      focused = false;
      updateFocus();
      break;
    case APP_CMD_RESUME:
      resumed = true;
      updateFocus();
      break;
    case APP_CMD_PAUSE:
      resumed = false;
      updateFocus();
      break;
    case APP_CMD_TERM_WINDOW:
      // The activity is only hidden. Rendering stops until NativeActivity provides a new window.
      hasWindow = false;
      break;
    case APP_CMD_DESTROY:
      hasWindow = false;
      if(mainWindow!=nullptr) {
        CloseEvent event;
        AndroidApi::dispatchClose(*mainWindow,event);
        }
      isExit.store(true);
      break;
    default:
      break;
    }
  }

bool AndroidApi::processKeyEvent(const void* input) {
  const auto    event  = static_cast<const AInputEvent*>(input);
  const int32_t code   = AKeyEvent_getKeyCode(event);
  const int32_t action = AKeyEvent_getAction(event);
  if(action!=AKEY_EVENT_ACTION_DOWN && action!=AKEY_EVENT_ACTION_UP)
    return false;
  const bool down = action==AKEY_EVENT_ACTION_DOWN;

  const int button = gamepadButton(code, isGamepadSource(AInputEvent_getSource(event)));
  if(button>=0) {
    selectPad(keyDevice, gamepadKeys, event);
    gamepadKeys.buttons[button] = down;
    return true;
    }
  if(code==AKEYCODE_BUTTON_L2 || code==AKEYCODE_BUTTON_R2) {
    selectPad(keyDevice, gamepadKeys, event);
    auto& trigger = code==AKEYCODE_BUTTON_L2 ? gamepadKeys.lt : gamepadKeys.rt;
    trigger = down ? 1.f : 0.f;
    return true;
    }
  if(code==AKEYCODE_BACK || code==AKEYCODE_VOLUME_UP || code==AKEYCODE_VOLUME_DOWN)
    return false;

  const int32_t meta = AKeyEvent_getMetaState(event);
  const Event::KeyType kt  = androidKey(code);
  const uint32_t       chr = translateChar(code,meta);
  if(kt==Event::K_NoKey && chr==0)
    return false;
  if(mainWindow==nullptr)
    return true;

  const bool cancelled = (AKeyEvent_getFlags(event) & AKEY_EVENT_FLAG_CANCELED)!=0;
  const auto type = down ? Event::KeyDown : (cancelled ? Event::KeyCancel : Event::KeyUp);
  KeyEvent e(kt, chr, translateModifiers(meta), type);
  uint32_t scan = uint32_t(AKeyEvent_getScanCode(event));
  if(scan==0)
    scan = 0x40000000u | uint32_t(code);
  if(kt==Event::K_NoKey) {
    // Character-only keys have no logical identity to share with a gamepad.
    if(down)
      dispatchKeyDown(*mainWindow,e,scan); else
      dispatchKeyUp(*mainWindow,e,scan);
    } else {
    const uint64_t source = (uint64_t(uint32_t(AInputEvent_getDeviceId(event)))<<32) | scan;
    dispatchInputKey(*mainWindow,e,source,AKeyEvent_getRepeatCount(event)>0);
    }
  return true;
  }

bool AndroidApi::processMouseEvent(const void* input) {
  const auto    event  = static_cast<const AInputEvent*>(input);
  const int32_t source = AInputEvent_getSource(event);
  const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  if(mainWindow==nullptr)
    return true;

  const bool relative = (source & AINPUT_SOURCE_MOUSE_RELATIVE)==AINPUT_SOURCE_MOUSE_RELATIVE;
  float dx = 0, dy = 0;
  const Point pos = AMotionEvent_getXY(event,0);
  if(relative) {
    dx = AMotionEvent_getX(event,0);
    dy = AMotionEvent_getY(event,0);
    } else {
    dx = AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_RELATIVE_X,0);
    dy = AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_RELATIVE_Y,0);
    if(dx==0 && dy==0 && mouseValid) {
      dx = float(pos.x-mouseLast.x);
      dy = float(pos.y-mouseLast.y);
      }
    mouseLast  = pos;
    mouseValid = true;
    }

  // The game reads mouse-look as the offset from the window centre (desktop recentres the cursor every event)
  const Rect  rect   = implClientRectOf();
  const Point center = Point(rect.w/2,rect.h/2);
  const Point at     = Point(center.x+int(dx), center.y+int(dy));

  switch(action) {
    case AMOTION_EVENT_ACTION_BUTTON_PRESS:
    case AMOTION_EVENT_ACTION_BUTTON_RELEASE: {
      // AMotionEvent_getActionButton needs API 33: compare the whole button state instead
      const int32_t  now     = AMotionEvent_getButtonState(event);
      const struct { int32_t bit; Event::MouseButton b; } map[] = {
        {AMOTION_EVENT_BUTTON_PRIMARY,   Event::ButtonLeft },
        {AMOTION_EVENT_BUTTON_SECONDARY, Event::ButtonRight},
        {AMOTION_EVENT_BUTTON_TERTIARY,  Event::ButtonMid  },
        {AMOTION_EVENT_BUTTON_BACK,      Event::ButtonBack },
        {AMOTION_EVENT_BUTTON_FORWARD,   Event::ButtonForward},
        };
      for(auto& m:map) {
        const bool was = (mousePressed & m.bit)!=0;
        const bool is  = (now     & m.bit)!=0;
        if(was==is)
          continue;
        MouseEvent e(center.x, center.y, m.b, Event::M_NoModifier, 0, 0, is ? Event::MouseDown : Event::MouseUp);
        if(is)
          SystemApi::dispatchMouseDown(*mainWindow,e); else
          SystemApi::dispatchMouseUp  (*mainWindow,e);
        }
      mousePressed = now;
      return true;
      }
    case AMOTION_EVENT_ACTION_SCROLL: {
      const float v = AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_VSCROLL,0);
      MouseEvent e(center.x, center.y, Event::ButtonNone, Event::M_NoModifier, int(v*120.f), 0, Event::MouseWheel);
      SystemApi::dispatchMouseWheel(*mainWindow,e);
      return true;
      }
    case AMOTION_EVENT_ACTION_HOVER_MOVE:
    case AMOTION_EVENT_ACTION_MOVE: {
      if(dx==0 && dy==0)
        return true;
      MouseEvent e(at.x, at.y, Event::ButtonNone, Event::M_NoModifier, 0, 0, Event::MouseMove);
      SystemApi::dispatchMouseMove(*mainWindow,e);
      return true;
      }
    }
  return true;
  }

int32_t AndroidApi::onInputEvent(const void* input) {
  if(externalPresentation && !externalInputFocus)
    return 0;
  const auto    event  = static_cast<const AInputEvent*>(input);
  const int32_t source = AInputEvent_getSource(event);
  if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY)
    return processKeyEvent(event) ? 1 : 0;
  if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_MOTION && isGamepadSource(source)) {
    auto axis = [event](int32_t a) { return AMotionEvent_getAxisValue(event,a,0); };
    selectPad(axisDevice, gamepadState, event);
    gamepadState.lx = axis(AMOTION_EVENT_AXIS_X);
    gamepadState.ly = axis(AMOTION_EVENT_AXIS_Y);
    gamepadState.rx = axis(AMOTION_EVENT_AXIS_Z);
    gamepadState.ry = axis(AMOTION_EVENT_AXIS_RZ);
    gamepadState.lt = std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER),axis(AMOTION_EVENT_AXIS_BRAKE));
    gamepadState.rt = std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER),axis(AMOTION_EVENT_AXIS_GAS));
    const float hx = axis(AMOTION_EVENT_AXIS_HAT_X);
    const float hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
    gamepadState.buttons[GamepadState::DpadLeft]  = hx<-0.5f;
    gamepadState.buttons[GamepadState::DpadRight] = hx> 0.5f;
    gamepadState.buttons[GamepadState::DpadUp]    = hy<-0.5f;
    gamepadState.buttons[GamepadState::DpadDown]  = hy> 0.5f;
    return 1;
    }
  if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_MOTION &&
     ((source & AINPUT_SOURCE_MOUSE)==AINPUT_SOURCE_MOUSE || (source & AINPUT_SOURCE_MOUSE_RELATIVE)==AINPUT_SOURCE_MOUSE_RELATIVE))
    return processMouseEvent(event) ? 1 : 0;
  if(AInputEvent_getType(event)!=AINPUT_EVENT_TYPE_MOTION ||
     (AInputEvent_getSource(event) & AINPUT_SOURCE_TOUCHSCREEN)!=AINPUT_SOURCE_TOUCHSCREEN)
    return 0;

  const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  const int32_t index  = AMotionEvent_getPointerIndex(event);


  switch(action) {
    case AMOTION_EVENT_ACTION_DOWN:
      // A new gesture: release touches whose end was never reported
      releaseAllTouches();
      pressTouch(AMotionEvent_getPointerId(event,index),AMotionEvent_getXY(event, index));
      break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
      pressTouch(AMotionEvent_getPointerId(event,index),AMotionEvent_getXY(event, index));
      break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
      updateTouch(AMotionEvent_getPointerId(event,index),AMotionEvent_getXY(event, index),true);
      break;
    case AMOTION_EVENT_ACTION_MOVE:
      for(size_t i=0; i<AMotionEvent_getPointerCount(event); ++i)
        updateTouch(AMotionEvent_getPointerId(event,i),AMotionEvent_getXY(event, i),false);
      break;
    case AMOTION_EVENT_ACTION_CANCEL:
      releaseAllTouches();
      break;
    default:
      return 0;
    }
  return 1;
  }

bool AndroidApi::processTouchEvents(AppCallBack& cb) {
  if(mainWindow==nullptr || isExit.load())
    return false;
  // Dispatch a copy: handlers may poll recursively and change touches
  for(size_t i=0; i<touches.size(); ++i) {
    auto& t = touches[i];
    if(t.down) {
      t.down     = false;
      t.moveLast = t.downPos;
      MouseEvent e(t.moveLast.x, t.moveLast.y, Event::ButtonLeft, Event::M_NoModifier, 0, int32_t(i), Event::MouseDown);
      SystemApi::dispatchMouseDown(*mainWindow,e);
      return true;
      }
    if(t.up) {
      t.up       = false;
      t.moveLast = t.movePos;
      MouseEvent e(t.moveLast.x, t.moveLast.y, Event::ButtonLeft, Event::M_NoModifier, 0, int32_t(i), Event::MouseUp);
      SystemApi::dispatchMouseUp(*mainWindow,e);
      return true;
      }
    if(t.moveLast!=t.movePos) {
      t.moveLast = t.movePos;
      MouseEvent e(t.moveLast.x, t.moveLast.y, Event::ButtonLeft, Event::M_NoModifier, 0, uint32_t(i), Event::MouseMove);
      SystemApi::dispatchMouseMove(*mainWindow,e);
      return true;
      }
    }
  return false;
  }

static void pollAndroid(android_app* state, int timeout) {
  int                  pending = 0;
  android_poll_source* source  = nullptr;
  while(ALooper_pollOnce(timeout,nullptr,&pending,reinterpret_cast<void**>(&source))>=0) {
    if(source!=nullptr)
      source->process(state,source);
    if(isExit.load())
      break;
    timeout = 0;
    }
  }

SystemApi::Window* AndroidApi::createAndroidWindow(Tempest::Window* owner) {
  if(mainWindow!=nullptr)
    return nullptr;
  app->onAppCmd = [](android_app* state, int32_t cmd) { onAppCmd(state,cmd); };
  app->onInputEvent = [](android_app*, AInputEvent* event) { return onInputEvent(event); };
  while(!hasWindow && !isExit.load() && app->destroyRequested==0)
    pollAndroid(app,-1);
  if(isExit.load() || app->destroyRequested!=0)
    return nullptr;
  mainWindow = owner;
  // NativeActivity replaces its ANativeWindow while the activity lives; the activity object identifies the window
  return reinterpret_cast<SystemApi::Window*>(app->activity->clazz);
  }

SystemApi::Window* AndroidApi::implCreateWindow(Tempest::Window* owner, uint32_t, uint32_t) {
  return createAndroidWindow(owner);
  }

SystemApi::Window* AndroidApi::implCreateWindow(Tempest::Window* owner, ShowMode) {
  return createAndroidWindow(owner);
  }

void AndroidApi::implDestroyWindow(SystemApi::Window*) {
  heldKeys.clear();
  externalInputFocus = true;
  mainWindow = nullptr;
  touches.clear();
  }

void AndroidApi::implExit() {
  isExit.store(true);
  ALooper_wake(app->looper);
  }

ANativeWindow* AndroidApi::nativeWindow(SystemApi::Window* w) {
  if(reinterpret_cast<jobject>(w)!=app->activity->clazz)
    return nullptr;
  return app->window;
  }

Rect AndroidApi::implClientRectOf() {
  if(externalPresentation)
    return Rect(0,0,externalW,externalH);
  if(app==nullptr || app->window==nullptr)
    return Rect();
  return Rect(0,0,ANativeWindow_getWidth(app->window),ANativeWindow_getHeight(app->window));
  }

Rect AndroidApi::implWindowClientRect(SystemApi::Window* w) {
  if(externalPresentation)
    return Rect(0,0,externalW,externalH);
  const auto window = nativeWindow(w);
  if(window==nullptr)
    return Rect();
  return Rect(0,0,ANativeWindow_getWidth(window),ANativeWindow_getHeight(window));
  }

float AndroidApi::implUiScale(SystemApi::Window*) {
  if(externalPresentation)
    return externalUiScale;
  const auto density = AConfiguration_getDensity(app->config);
  if(density==ACONFIGURATION_DENSITY_DEFAULT || density==ACONFIGURATION_DENSITY_ANY || density==ACONFIGURATION_DENSITY_NONE)
    return 1.f;
  return float(density)/ACONFIGURATION_DENSITY_MEDIUM;
  }

bool AndroidApi::implSetAsFullscreen(SystemApi::Window*, bool) {
  // TODO: toggle Android immersive mode.
  return false;
  }

bool AndroidApi::implIsFullscreen(SystemApi::Window*) {
  // TODO: query Android immersive mode.
  return true;
  }

void AndroidApi::implSetCursorPosition(SystemApi::Window*, int, int) {
  }

void AndroidApi::implShowCursor(SystemApi::Window*, CursorShape) {
  }

bool AndroidApi::implIsRunning() {
  return !isExit.load();
  }

int AndroidApi::implExec(AppCallBack& cb) {
  while(!isExit.load()) {
    implProcessEvents(cb);
    }
  return 0;
  }

void AndroidApi::implProcessEvents(AppCallBack& cb) {
  if(isExit.load())
    return;

  if(processTouchEvents(cb))
    return;

  int                  pending = 0;
  android_poll_source* source  = nullptr;
  const bool          visible = externalPresentation ? resumed : (active && hasWindow);
  // OpenXR session events arrive outside the Android looper. Keep its
  // frame callback alive to handle STOPPING even while the activity sleeps.
  const int timeoutMs = visible ? 0 : (externalPresentation ? 100 : -1);
  if(ALooper_pollOnce(timeoutMs,nullptr,&pending,reinterpret_cast<void**>(&source))>=0) {
    if (source != nullptr)
      source->process(app, source);
    return;
    }

  if((!visible && !externalPresentation) || isExit.load())
    return;

  if(visible && cb.onTimer()==0)
    std::this_thread::yield();

  if(mainWindow!=nullptr)
    dispatchRender(*mainWindow);
  }

void AndroidApi::implSetWindowTitle(SystemApi::Window*, const char*) {
  // TODO: update the activity title through JNI.
  }

static int runMain() {
  Dl_info module = {};
  if(dladdr(reinterpret_cast<void*>(&android_main),&module)==0) {
    Log::e("Unable to locate the application library");
    return EXIT_FAILURE;
    }
  void* self = dlopen(module.dli_fname,RTLD_NOW);
  if(self==nullptr) {
    const char* error = dlerror();
    Log::e("Unable to open the application library: ",error==nullptr ? "unknown error" : error);
    return EXIT_FAILURE;
    }
  using Main = int(*)(int,char**);
  auto entry = reinterpret_cast<Main>(dlsym(self,"main"));
  if(entry==nullptr) {
    const char* error = dlerror();
    Log::e("Unable to find the application entry point: ",error==nullptr ? "unknown error" : error);
    dlclose(self);
    return EXIT_FAILURE;
    }
  // NativeActivity owns the library for the duration of android_main.
  dlclose(self);
  char  arg0[] = "app";
  char* argv[] = {arg0,nullptr};
  return entry(1,argv);
  }

extern "C" void android_main(android_app* state) {
  static std::atomic_flag started = ATOMIC_FLAG_INIT;
  if(started.test_and_set()) {
    ANativeActivity_finish(state->activity);
    while(state->destroyRequested==0)
      pollAndroid(state,-1);
    return;
    }
  app = state;
  int result = EXIT_FAILURE;
  try {
    result = runMain();
    }
  catch(const std::exception& e) {
    Log::e("Unhandled native exception: ",e.what());
    }
  catch(...) {
    Log::e("Unhandled native exception");
    }

  if(app->destroyRequested==0)
    ANativeActivity_finish(app->activity);
  // Re-entering main would reuse application statics from the previous run.
  // The application has unwound; process-wide destructors can race Android runtime threads.
  std::_Exit(result);
  }

#endif
