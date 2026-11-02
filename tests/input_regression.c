/* Host HID/JNI adapters around the unmodified input functions from main.c. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
static int test_clock_gettime(int clock_id, struct timespec *ts) {
  static uint64_t ns = 1000000000ULL;
  (void)clock_id;
  ns += 8333333;
  ts->tv_sec = (time_t)(ns / 1000000000ULL);
  ts->tv_nsec = (long)(ns % 1000000000ULL);
  return 0;
}
#define clock_gettime test_clock_gettime

enum {
  HidNpadButton_A = 1ULL << 0, HidNpadButton_B = 1ULL << 1,
  HidNpadButton_X = 1ULL << 2, HidNpadButton_Y = 1ULL << 3,
  HidNpadButton_StickL = 1ULL << 4, HidNpadButton_StickR = 1ULL << 5,
  HidNpadButton_L = 1ULL << 6, HidNpadButton_R = 1ULL << 7,
  HidNpadButton_ZL = 1ULL << 8, HidNpadButton_ZR = 1ULL << 9,
  HidNpadButton_Plus = 1ULL << 10, HidNpadButton_Minus = 1ULL << 11,
  HidNpadButton_Left = 1ULL << 12, HidNpadButton_Up = 1ULL << 13,
  HidNpadButton_Right = 1ULL << 14, HidNpadButton_Down = 1ULL << 15,
  HidNpadStyleSet_NpadStandard = 0
};
typedef struct { int unused; } PadState;
typedef struct { int x, y; } HidAnalogStickState;
typedef struct { int count; struct { int x, y; } touches[16]; } HidTouchScreenState;
static uint64_t held, down, up, previous_held;
static HidAnalogStickState stick;
static HidTouchScreenState touch;
static void padConfigureInput(int n, int style) { (void)n; (void)style; }
static void padInitializeDefault(PadState *pad) { (void)pad; }
static void hidInitializeTouchScreen(void) {}
static void padUpdate(PadState *pad) { (void)pad; }
static uint64_t padGetButtonsDown(PadState *pad) { (void)pad; return down; }
static uint64_t padGetButtonsUp(PadState *pad) { (void)pad; return up; }
static uint64_t padGetButtons(PadState *pad) { (void)pad; return held; }
static HidAnalogStickState padGetStickPos(PadState *pad, int index) {
  (void)pad; (void)index; return stick;
}
static int hidGetTouchScreenStates(HidTouchScreenState *state, int n) {
  (void)n; *state = touch; return 1;
}

typedef uint8_t jboolean;
typedef struct {
  void (*touch)(void *, void *, int, int64_t, int, float, float, float, float);
  void (*key)(void *, void *, int, jboolean, jboolean, int);
  void (*motion)(void *, void *, int, int, float, float);
} MortarApi;
static int screen_width = 1280, screen_height = 720;
static int keys[256], pointer_down, touch_events;
static float pointer_x, pointer_y, axis_x, axis_y;
static void receive_key(void *env, void *obj, int key, jboolean pressed,
                        jboolean repeat, int device) {
  (void)env; (void)obj; (void)repeat; (void)device;
  assert(key >= 0 && key < 256);
  keys[key] = pressed;
}
static void receive_touch(void *env, void *obj, int action, int64_t timestamp,
                          int id, float x, float y, float pressure, float size) {
  (void)env; (void)obj; (void)timestamp; (void)pressure; (void)size;
  assert(id == 0);
  assert(action == 0 ? !pointer_down : pointer_down);
  pointer_down = action != 1;
  pointer_x = x; pointer_y = y; touch_events++;
}
static void receive_motion(void *env, void *obj, int device, int axis,
                           float x, float y) {
  (void)env; (void)obj; (void)device; (void)axis;
  axis_x = x; axis_y = y;
}
static const MortarApi api = {receive_touch, receive_key, receive_motion};

#include "input_under_test.inc"

static void reset(InputState *input) {
  held = down = up = previous_held = 0;
  stick = (HidAnalogStickState){0, 0};
  memset(&touch, 0, sizeof touch);
  memset(keys, 0, sizeof keys);
  screen_width = 1280; screen_height = 720;
  pointer_down = touch_events = 0;
  pointer_x = pointer_y = axis_x = axis_y = 0.0f;
  input_init(input);
  assert(!input->mouse_mode && !input->cursor_visible);
}
static void frame(InputState *input, uint64_t buttons, int sx, int sy,
                   int physical, int x, int y) {
  held = buttons; down = held & ~previous_held; up = previous_held & ~held;
  stick = (HidAnalogStickState){sx, sy};
  touch.count = physical; touch.touches[0].x = x; touch.touches[0].y = y;
  input_update(input, &api, NULL, NULL);
  previous_held = held;
  assert(input->mouse_mode || !input->cursor_visible);
}
static void mouse_mode(InputState *input) {
  frame(input, HidNpadButton_ZL, 0, 0, 0, 0, 0);
  frame(input, 0, 0, 0, 0, 0, 0);
  assert(input->mouse_mode && input->cursor_visible && !keys[104]);
}
static void near(float actual, float expected) {
  assert(fabsf(actual - expected) < 0.00001f);
}

int main(void) {
  InputState input;
  reset(&input);
  frame(&input, HidNpadButton_A, 30000, 0, 0, 0, 0);
  assert(keys[96] && !pointer_down);
  near(input.cursor_x, 0.5f);
  frame(&input, 0, 0, 0, 0, 0, 0);
  assert(!keys[96]);

  const int sx[] = {0, 0, -30000, 30000};
  const int sy[] = {30000, -30000, 0, 0};
  const uint64_t directions[] = {HidNpadButton_Up, HidNpadButton_Down,
                                HidNpadButton_Left, HidNpadButton_Right};
  for (int i = 0; i < 4; ++i) {
    /* Leaving controller mode releases every virtual direction. */
    reset(&input);
    frame(&input, 0, sx[i], sy[i], 0, 0, 0);
    assert(keys[19 + i]);
    frame(&input, HidNpadButton_ZL, sx[i], sy[i], 0, 0, 0);
    frame(&input, 0, 0, 0, 0, 0, 0);
    assert(!keys[19 + i]);
    /* Leaving mouse mode clears persistent axes, then restores navigation. */
    frame(&input, 0, sx[i], sy[i], 0, 0, 0);
    assert(axis_x != 0.0f || axis_y != 0.0f);
    frame(&input, HidNpadButton_ZL, sx[i], sy[i], 0, 0, 0);
    near(axis_x, 0.0f); near(axis_y, 0.0f);
    assert(keys[19 + i]);
    frame(&input, 0, 0, 0, 0, 0, 0);
    assert(!keys[19 + i]);

    /* Each physical/virtual overlap order must preserve the remaining hold. */
    reset(&input);
    frame(&input, directions[i], 0, 0, 0, 0, 0);
    frame(&input, directions[i], sx[i], sy[i], 0, 0, 0);
    frame(&input, directions[i], 0, 0, 0, 0, 0);
    assert(keys[19 + i]);
    frame(&input, HidNpadButton_ZL | directions[i], 0, 0, 0, 0, 0);
    assert(keys[19 + i]);
    frame(&input, 0, 0, 0, 0, 0, 0);
    assert(!keys[19 + i]);
    reset(&input);
    frame(&input, 0, sx[i], sy[i], 0, 0, 0);
    frame(&input, directions[i], sx[i], sy[i], 0, 0, 0);
    frame(&input, 0, sx[i], sy[i], 0, 0, 0);
    assert(keys[19 + i]);
    frame(&input, 0, 0, 0, 0, 0, 0);
    assert(!keys[19 + i]);
  }

  const int resolutions[][2] = {{640,360}, {960,540}, {1280,720}, {1920,1080}};
  const int points[][2] = {{320,180}, {640,360}, {1279,719}};
  for (int mode = 0; mode < 2; ++mode) {
    for (size_t r = 0; r < sizeof resolutions / sizeof resolutions[0]; ++r) {
      reset(&input);
      if (mode) mouse_mode(&input);
      screen_width = resolutions[r][0]; screen_height = resolutions[r][1];
      for (size_t p = 0; p < sizeof points / sizeof points[0]; ++p) {
        frame(&input, 0, 0, 0, 1, points[p][0], points[p][1]);
        near(pointer_x, (float)points[p][0] / 1280.0f);
        near(pointer_y, (float)points[p][1] / 720.0f);
        assert(pointer_down && !input.cursor_visible);
        /* A mode toggle must not interrupt a physical gesture. */
        const int before = touch_events;
        frame(&input, HidNpadButton_ZL, 0, 0, 1, points[p][0], points[p][1]);
        assert(pointer_down && touch_events == before + 1);
        frame(&input, 0, 0, 0, 0, 0, 0);
        assert(!pointer_down);
        frame(&input, HidNpadButton_ZL, 0, 0, 0, 0, 0);
        frame(&input, 0, 0, 0, 0, 0, 0);
      }
    }
  }

  reset(&input);
  mouse_mode(&input);
  frame(&input, HidNpadButton_A, 0, 0, 0, 0, 0);
  assert(pointer_down);
  frame(&input, HidNpadButton_ZL | HidNpadButton_A, 0, 0, 0, 0, 0);
  assert(!pointer_down && !input.cursor_visible);

  reset(&input);
  mouse_mode(&input);
  frame(&input, HidNpadButton_Minus, 0, 0, 0, 0, 0);
  assert(pointer_down && !keys[109]);
  near(pointer_x, 0.5f); near(pointer_y, 719.0f / 720.0f);
  frame(&input, HidNpadButton_Minus | HidNpadButton_A, 0, 0, 0, 0, 0);
  frame(&input, HidNpadButton_Minus, 0, 0, 0, 0, 0);
  assert(pointer_down);
  near(pointer_y, 719.0f / 720.0f);
  frame(&input, HidNpadButton_Minus, 0, 0, 1, 320, 180);
  near(pointer_x, 0.25f); near(pointer_y, 0.25f);
  frame(&input, HidNpadButton_Minus, 0, 0, 0, 0, 0);
  assert(pointer_down);
  near(pointer_y, 719.0f / 720.0f);
  frame(&input, 0, 0, 0, 0, 0, 0);
  assert(!pointer_down);
  puts("PASS: mode transitions, D-pad ownership, resolution-independent touch, cursor visibility and touch ownership");
  return 0;
}
