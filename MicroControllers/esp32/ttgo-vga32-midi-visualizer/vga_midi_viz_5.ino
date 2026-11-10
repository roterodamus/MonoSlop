// ---------------------------------------------------------------
//  TTGO VGA32 v1.4  -  Abstract BLE-MIDI Visualizer
//  MIDI ch 1-8 -> 8 independent shape layers, 32 shapes
//  MIDI ch 9   -> word layer (CC20 picks one of 128 words)
//  MIDI channel 16 -> background color + preset buttons
//  Presets: Program Change (any channel) selects slot 0-31,
//           ch16 CC23 = LOAD, ch16 CC24 = SAVE (value >= 64)
//           ch16 CC25 = slot bar on screen: >= 64 shows it for 2 s, < 64 hides it (default)
//  Button on IO36 toggles BLE-MIDI mode:
//    DEVICE   : advertises as "VGA-MIDI-Viz" (iPad / computer connects)  -> red dot
//    KEYBOARD : scans for and connects to BLE-MIDI keyboards            -> yellow dot
// ---------------------------------------------------------------
#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include "fabgl.h"

#define DOUBLE_BUFFER 1      // set to 0 if you run out of RAM (will flicker)
#define DEBUG_CC      1      // prints every CC / program change to Serial (115200)
#define BTN_PIN       36     // input-only pin, no internal pull resistor

// ---------- Display ----------
fabgl::VGAController DisplayController;
fabgl::Canvas canvas(&DisplayController);
const int W = 320, H = 240;

// ---------- BLE MIDI ----------
static NimBLEUUID MIDI_SVC("03b80e5a-ede8-4b33-a751-6ce34ec4c700");
static NimBLEUUID MIDI_CHR("7772e5db-3868-4112-a1a9-f2669d106bf3");

enum Mode : uint8_t { MODE_DEVICE = 0, MODE_KEYBOARD = 1 };
Mode mode = MODE_DEVICE;
Preferences prefs;

NimBLEServer*         bleServer = nullptr;
NimBLEAdvertising*    adv       = nullptr;
NimBLEClient*         client    = nullptr;

NimBLEAddress* volatile targetAddr = nullptr;   // keyboard found by the scanner
volatile bool          haveTarget  = false;

// MIDI messages are queued from the BLE task and handled in loop()
struct MidiMsg { uint8_t status, d1, d2; };
QueueHandle_t midiQ;

static inline void pushMidi(uint8_t s, uint8_t a, uint8_t b) {
  MidiMsg m = { s, a, b };
  xQueueSend(midiQ, &m, 0);
}

// Parse one BLE-MIDI packet (timestamps, running status, multiple messages)
void parseBleMidi(const uint8_t* d, size_t len) {
  if (len < 2) return;
  uint8_t status = 0;
  size_t i = 1;                                  // skip header byte
  while (i < len) {
    uint8_t b = d[i];
    if (b & 0x80) {                              // timestamp byte
      i++;
      if (i >= len) break;
      b = d[i];
      if (b & 0x80) {                            // a status byte follows
        if (b == 0xF0) {                         // sysex: skip it
          while (i < len && d[i] != 0xF7) i++;
          i++; status = 0; continue;
        }
        i++;
        if (b >= 0xF0) { status = 0; continue; } // system / realtime: ignore
        status = b;
      }                                          // else: running status
    }
    if (status == 0) { i++; continue; }          // stray data byte
    uint8_t hi   = status & 0xF0;
    uint8_t need = (hi == 0xC0 || hi == 0xD0) ? 1 : 2;
    uint8_t data[2] = { 0, 0 };
    uint8_t got = 0;
    while (got < need && i < len && !(d[i] & 0x80)) data[got++] = d[i++];
    if (got == need) pushMidi(status, data[0], data[1]);
  }
}

// Data written by an iPad / computer in DEVICE mode
class ChrCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    auto v = c->getValue();
    parseBleMidi((const uint8_t*)v.data(), v.size());
  }
};
ChrCB chrCB;

// Notifications from a keyboard in KEYBOARD mode
void notifyCB(NimBLERemoteCharacteristic* c, uint8_t* data, size_t len, bool isNotify) {
  parseBleMidi(data, len);
}

// Scanner: remember the first device that advertises the BLE-MIDI service
class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    if (!haveTarget && d->isAdvertisingService(MIDI_SVC)) {
      targetAddr = new NimBLEAddress(d->getAddress());
      haveTarget = true;
      NimBLEDevice::getScan()->stop();
    }
  }
};
ScanCB scanCB;

bool isConnected() {
  if (mode == MODE_DEVICE) return bleServer && bleServer->getConnectedCount() > 0;
  return client && client->isConnected();
}

// ---------- Model ----------
constexpr int NUM_CH     = 9;       // ch1-8 shapes, ch9 words
constexpr int WORD_CH    = 8;       // index of the word layer (MIDI channel 9)
constexpr int NUM_WORDS  = 128;
constexpr int MAX_VOICES = 10;
constexpr int CC_BASE    = 20;      // CC20..CC31 = per-channel controls
constexpr int NUM_CC_BLK = 12;      // contiguous block
constexpr int NUM_CC     = 14;      // + 2 extra (CC85, CC86)
constexpr int CC_ORBMODE = 85;      // orbit angle mode
constexpr int CC_ORBSPD  = 86;      // orbit auto-rotate speed
constexpr int NUM_SHAPES = 32;
constexpr int BG_CHANNEL = 16;      // MIDI channel for background + preset control
constexpr int CC_LOAD    = 23;      // ch16: load selected preset slot
constexpr int CC_SAVE    = 24;      // ch16: save to selected preset slot
constexpr int CC_SLOTBAR = 25;      // ch16: slot bar on screen (>=64 = show 2 s, <64 = hidden)
constexpr int NUM_SLOTS  = 32;      // program change 0..31
const float QPI = PI / 4.0f;

enum Param { P_SHAPE, P_SPIN, P_HUE, P_SIZE, P_RELEASE, P_COPIES,
             P_ORBIT, P_STYLE, P_SPREAD, P_EXPAND, P_X, P_Y,
             P_ORBMODE, P_ORBSPD };

// shape zones for CC20 (32 zones, 4 CC values each: value = shape number * 4)
//  0 circle    1 triangle   2 square    3 diamond   4 pentagon  5 hexagon
//  6 star      7 cross      8 heart     9 crescent 10 line     11 sparkle
// 12 spade    13 club      14 chevron  15 ring     16 gear     17 crown
// 18 shield   19 hourglass 20 bolt     21 arrow    22 flower   23 bell
// 24 note     25 wrench    26 envelope 27 acid     28 infinity 29 dollar
// 30 euro     31 yen

struct Voice {
  bool  active = false, held = false;
  uint8_t note = 0;
  float vel = 0, life = 0, age = 0;
};

struct Chan {
  uint8_t cc[NUM_CC];
  float spin = 0;        // shape self-rotation
  float orbAngle = 0;    // auto-rotation around the orbit
  float bend = 0;
  Voice v[MAX_VOICES];
};

Chan chans[NUM_CH];

// Background (MIDI ch16): CC20 = hue, CC21 = saturation, CC22 = brightness
uint8_t bgHue = 85, bgSat = 127, bgVal = 0;     // starts black
RGB888  bgCol(0, 0, 0);                         // current background (set in render)

//                         shape spin hue size rel cop orb sty spr exp  x   y  omode ospd
const uint8_t DEFAULTS[NUM_CH][NUM_CC] = {
  {   2, 64,   0, 60, 50, 30, 45, 100, 30, 25, 64, 64, 110, 70 },  // 1 circle
  {  14, 64,  15, 55, 45, 50, 50,  70, 40, 10, 64, 64, 110, 54 },  // 2 diamond
  {  34, 64,  30, 50, 60, 40, 40,  50, 20, 30, 64, 64, 110, 82 },  // 3 heart
  {  26, 64,  45, 65, 40, 60, 55, 110, 50, 15, 64, 64, 110, 46 },  // 4 star
  {  38, 64,  60, 45, 70, 20, 35,  90, 25, 40, 64, 64, 110, 92 },  // 5 crescent
  {  50, 64,  75, 50, 55, 45, 60,  80, 35, 20, 64, 64, 110, 40 },  // 6 spade
  {  54, 64,  90, 55, 50, 35, 30, 120, 30, 30, 64, 64, 110, 88 },  // 7 club
  {  46, 64, 105, 60, 65, 25, 50,  60, 45, 35, 64, 64, 110, 60 },  // 8 sparkle
  {   0, 64, 120, 50, 55, 10, 40,   0, 60, 20, 64, 64, 110, 66 },  // 9 words (ACID)
};

// ---------- Presets ----------
struct Preset {
  uint8_t cc[NUM_CH][NUM_CC];
  uint8_t bg[3];
};

Preferences pre;
uint32_t slotUsed  = 0;        // bitmask of slots that hold data
int      curSlot   = 0;        // selected by program change
uint32_t slotUntil = 0;        // slot bar visible until this millis()
uint8_t  slotKind  = 0;        // 0 selected, 1 loaded, 2 saved, 3 error / empty
bool     slotBarEnabled = false;   // default: always hidden (ch16 CC25 >= 64 enables)

void showSlots(uint8_t kind) {
  slotKind  = kind;
  slotUntil = millis() + 2000;
}

void saveSlot(int n) {
  Preset p;
  for (int i = 0; i < NUM_CH; i++) memcpy(p.cc[i], chans[i].cc, NUM_CC);
  p.bg[0] = bgHue; p.bg[1] = bgSat; p.bg[2] = bgVal;
  char key[8];
  snprintf(key, sizeof key, "s%d", n);
  if (pre.putBytes(key, &p, sizeof p) == sizeof p) {
    slotUsed |= (1u << n);
    pre.putUInt("used", slotUsed);
    showSlots(2);
  } else {
    showSlots(3);
  }
}

void loadSlot(int n) {
  if (!(slotUsed & (1u << n))) { showSlots(3); return; }   // empty slot
  char key[8];
  snprintf(key, sizeof key, "s%d", n);
  uint8_t buf[sizeof(Preset)];
  int len = (int)pre.getBytesLength(key);
  if (len < 3 + NUM_CC || len > (int)sizeof buf || (len - 3) % NUM_CC != 0 ||
      (int)pre.getBytes(key, buf, len) != len) { showSlots(3); return; }
  int nch = (len - 3) / NUM_CC;              // presets saved before the word layer hold 8 channels
  for (int i = 0; i < nch && i < NUM_CH; i++)
    for (int j = 0; j < NUM_CC; j++) chans[i].cc[j] = buf[i * NUM_CC + j] & 0x7F;
  const uint8_t* bg = buf + nch * NUM_CC;
  bgHue = bg[0] & 0x7F; bgSat = bg[1] & 0x7F; bgVal = bg[2] & 0x7F;
  showSlots(1);
}

void drawSlotBar() {
  if (!slotBarEnabled || (int32_t)(slotUntil - millis()) <= 0) return;
  const int cw = 9, x0 = (W - NUM_SLOTS * cw) / 2, y = H - 12;
  for (int i = 0; i < NUM_SLOTS; i++) {
    int x = x0 + i * cw;
    bool used = (slotUsed >> i) & 1;
    if (i == curSlot) {
      RGB888 c = slotKind == 1 ? RGB888(0, 255, 0)
               : slotKind == 2 ? RGB888(255, 0, 255)
               : slotKind == 3 ? RGB888(255, 0, 0)
                               : RGB888(0, 255, 255);
      canvas.setBrushColor(c);
      canvas.fillRectangle(x, y - 3, x + cw - 3, y + 9);
    } else if (used) {
      canvas.setBrushColor(RGB888(120, 120, 120));
      canvas.fillRectangle(x, y, x + cw - 3, y + 6);
    } else {
      canvas.setPenColor(RGB888(70, 70, 70));
      canvas.drawRectangle(x, y, x + cw - 3, y + 6);
    }
  }
}

// ---------- Helpers ----------
RGB888 hsv(float h, float s, float v) {
  h -= floorf(h);
  float hh = h * 6.0f;
  int i = (int)hh;
  float f = hh - i;
  float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  float r, g, b;
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return RGB888((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

// rotate local point (x,y) by (c,s) and translate to (cx,cy)
static inline Point rp(float cx, float cy, float x, float y, float c, float s) {
  return Point((int)(cx + x * c - y * s), (int)(cy + x * s + y * c));
}

void outline(const Point* p, int n) {
  for (int i = 0; i < n; i++) {
    const Point& a = p[i];
    const Point& b = p[(i + 1) % n];
    canvas.drawLine(a.X, a.Y, b.X, b.Y);
  }
}

// fill modes: 0 = convex polygon
//             1 = triangle fan (concave shapes), centre point stored in p[n]
//             2 = strip between two arcs (crescent)
void drawPoly(const Point* p, int n, int mode, int style, RGB888 col, RGB888 dim) {
  if (style != 0) {                          // filled
    canvas.setBrushColor(style == 2 ? dim : col);
    if (mode == 1) {
      for (int i = 0; i < n; i++) {
        Point tri[3] = { p[n], p[i], p[(i + 1) % n] };
        canvas.fillPath(tri, 3);
      }
    } else if (mode == 2) {
      for (int k = 0; k < n / 2 - 1; k++) {
        Point quad[4] = { p[k], p[k + 1], p[n - 2 - k], p[n - 1 - k] };
        canvas.fillPath(quad, 4);
      }
    } else {
      canvas.fillPath(p, n);
    }
  }
  if (style != 1) {                          // outlined
    canvas.setPenColor(col);
    outline(p, n);
  }
}

// fill only (convex piece) / outline only (any polygon): for composite shapes
void fillP(const Point* p, int n, int style, RGB888 col, RGB888 dim) {
  if (style == 0) return;
  canvas.setBrushColor(style == 2 ? dim : col);
  canvas.fillPath(p, n);
}

void strokeP(const Point* p, int n, int style, RGB888 col) {
  if (style == 1) return;
  canvas.setPenColor(col);
  outline(p, n);
}

// small circular cut-out painted in the background colour
void hole(float cx, float cy, float rad, int style, RGB888 col) {
  int d = max(3, (int)(rad * 2));
  if (style != 0) { canvas.setBrushColor(bgCol); canvas.fillEllipse((int)cx, (int)cy, d, d); }
  if (style != 1) { canvas.setPenColor(col);     canvas.drawEllipse((int)cx, (int)cy, d, d); }
}

// thick line segment (used by glyph shapes). fc = fill colour, lc = centre-line colour
void thickSeg(const Point& a, const Point& b, float th, int style, RGB888 fc, RGB888 lc) {
  float dx = b.X - a.X, dy = b.Y - a.Y;
  float len = sqrtf(dx * dx + dy * dy);
  if (style != 0 && len > 0.5f) {
    float ux = dx / len, uy = dy / len;
    float e = th * 0.3f, h = th * 0.5f;       // extend ends a little to close joints
    float nx = -uy * h, ny = ux * h;
    float ax = a.X - ux * e, ay = a.Y - uy * e;
    float bx = b.X + ux * e, by = b.Y + uy * e;
    Point q[4] = { Point((int)(ax + nx), (int)(ay + ny)),
                   Point((int)(bx + nx), (int)(by + ny)),
                   Point((int)(bx - nx), (int)(by - ny)),
                   Point((int)(ax - nx), (int)(ay - ny)) };
    canvas.setBrushColor(fc);
    canvas.fillPath(q, 4);
  }
  if (style != 1) {
    canvas.setPenColor(lc);
    canvas.drawLine(a.X, a.Y, b.X, b.Y);
  }
}

void ngon(Point* out, int n, float cx, float cy, float r, float rot) {
  for (int i = 0; i < n; i++) {
    float a = rot + i * TWO_PI / n;
    out[i] = Point((int)(cx + cosf(a) * r), (int)(cy + sinf(a) * r));
  }
}

void bar(Point* out, float cx, float cy, float halfLen, float halfThick, float rot) {
  float c = cosf(rot), s = sinf(rot);
  float dx[4] = { -halfLen,  halfLen, halfLen, -halfLen };
  float dy[4] = { -halfThick, -halfThick, halfThick, halfThick };
  for (int i = 0; i < 4; i++) out[i] = rp(cx, cy, dx[i], dy[i], c, s);
}

// lemniscate (infinity) lookup table, filled in setup()
constexpr int LEM_N = 24;
float lemX[LEM_N], lemY[LEM_N];

void initTables() {
  for (int i = 0; i < LEM_N; i++) {
    float t = i * TWO_PI / LEM_N;
    float st = sinf(t);
    float den = 1.0f + st * st;
    lemX[i] = 0.98f * cosf(t) / den;
    lemY[i] = 0.98f * st * cosf(t) / den * 1.6f;
  }
}

void drawShape(int type, float cx, float cy, float r, float rot,
               int style, RGB888 col, RGB888 dim) {
  Point pts[32];
  float c = cosf(rot), s = sinf(rot);
  RGB888 fc = (style == 2) ? dim : col;
  auto PT = [&](float x, float y) -> Point { return rp(cx, cy, x * r, y * r, c, s); };

  switch (type) {
    case 0: {                                // circle
      int d = max(3, (int)(r * 2));
      if (style != 0) { canvas.setBrushColor(fc);  canvas.fillEllipse((int)cx, (int)cy, d, d); }
      if (style != 1) { canvas.setPenColor(col);   canvas.drawEllipse((int)cx, (int)cy, d, d); }
      break;
    }
    case 1:                                  // triangle
      ngon(pts, 3, cx, cy, r, rot);
      drawPoly(pts, 3, 0, style, col, dim);
      break;
    case 2:                                  // square
      ngon(pts, 4, cx, cy, r, rot + QPI);
      drawPoly(pts, 4, 0, style, col, dim);
      break;
    case 3:                                  // diamond
      pts[0] = rp(cx, cy,  0,         -r, c, s);
      pts[1] = rp(cx, cy,  r * 0.6f,   0, c, s);
      pts[2] = rp(cx, cy,  0,          r, c, s);
      pts[3] = rp(cx, cy, -r * 0.6f,   0, c, s);
      drawPoly(pts, 4, 0, style, col, dim);
      break;
    case 4:                                  // pentagon
      ngon(pts, 5, cx, cy, r, rot);
      drawPoly(pts, 5, 0, style, col, dim);
      break;
    case 5:                                  // hexagon
      ngon(pts, 6, cx, cy, r, rot);
      drawPoly(pts, 6, 0, style, col, dim);
      break;
    case 6: {                                // 5-point star
      for (int i = 0; i < 10; i++) {
        float a = rot + i * TWO_PI / 10;
        float rr = (i & 1) ? r * 0.4f : r;
        pts[i] = Point((int)(cx + cosf(a) * rr), (int)(cy + sinf(a) * rr));
      }
      pts[10] = Point((int)cx, (int)cy);
      drawPoly(pts, 10, 1, style, col, dim);
      break;
    }
    case 7:                                  // cross (plus)
      bar(pts, cx, cy, r, max(1.5f, r * 0.18f), rot);
      drawPoly(pts, 4, 0, style, col, dim);
      bar(pts, cx, cy, r, max(1.5f, r * 0.18f), rot + HALF_PI);
      drawPoly(pts, 4, 0, style, col, dim);
      break;
    case 8: {                                // heart
      const int N = 24;
      float k = r / 17.0f;
      for (int i = 0; i < N; i++) {
        float t  = i * TWO_PI / N;
        float st = sinf(t);
        float hx = 16.0f * st * st * st;
        float hy = 13.0f * cosf(t) - 5.0f * cosf(2 * t)
                 - 2.0f * cosf(3 * t) - cosf(4 * t);
        pts[i] = rp(cx, cy, hx * k, -(hy + 2.5f) * k, c, s);
      }
      pts[N] = Point((int)cx, (int)cy);
      drawPoly(pts, N, 1, style, col, dim);
      break;
    }
    case 9: {                                // crescent
      const int N = 10;
      const float d = 0.45f, ri = 0.8f;
      float ix = (d * d + 1.0f - ri * ri) / (2.0f * d);
      float iy = sqrtf(1.0f - ix * ix);
      float th = atan2f(iy, ix);
      float ph = atan2f(iy, ix - d);
      for (int i = 0; i <= N; i++) {
        float f  = i / (float)N;
        float a  = th + f * (TWO_PI - 2 * th);
        float b  = ph + f * (TWO_PI - 2 * ph);
        float ox = cosf(a) + 0.19f,          iX = d + ri * cosf(b) + 0.19f;
        float oy = sinf(a),                  iY = ri * sinf(b);
        pts[i]             = rp(cx, cy, ox * r, oy * r, c, s);
        pts[2 * N + 1 - i] = rp(cx, cy, iX * r, iY * r, c, s);
      }
      drawPoly(pts, 2 * N + 2, 2, style, col, dim);
      break;
    }
    case 10:                                 // line
      bar(pts, cx, cy, r, max(1.5f, r * 0.12f), rot);
      drawPoly(pts, 4, 0, style, col, dim);
      break;
    case 11: {                               // 4-point sparkle
      for (int i = 0; i < 8; i++) {
        float a  = rot + i * TWO_PI / 8 - HALF_PI;
        float rr = (i & 1) ? r * 0.25f : r;
        pts[i] = Point((int)(cx + cosf(a) * rr), (int)(cy + sinf(a) * rr));
      }
      pts[8] = Point((int)cx, (int)cy);
      drawPoly(pts, 8, 1, style, col, dim);
      break;
    }
    case 12: {                               // spade: upside-down heart + stem
      const int N = 24;
      float k = r / 17.0f;
      for (int i = 0; i < N; i++) {
        float t  = i * TWO_PI / N;
        float st = sinf(t);
        float hx = 16.0f * st * st * st;
        float hy = 13.0f * cosf(t) - 5.0f * cosf(2 * t)
                 - 2.0f * cosf(3 * t) - cosf(4 * t);
        pts[i] = rp(cx, cy, hx * k * 0.8f, (hy + 2.5f) * k * 0.8f - 0.15f * r, c, s);
      }
      pts[N] = rp(cx, cy, 0, -0.1f * r, c, s);               // fan centre
      drawPoly(pts, N, 1, style, col, dim);
      Point stem[3] = { rp(cx, cy, -0.3f * r, 0.9f * r, c, s),
                        rp(cx, cy,  0.3f * r, 0.9f * r, c, s),
                        rp(cx, cy,  0,        0.3f * r, c, s) };
      drawPoly(stem, 3, 0, style, col, dim);
      break;
    }
    case 13: {                               // club: three circles + stem
      float rr = r * 0.4f;
      float lx[3] = { 0, -0.42f * r, 0.42f * r };
      float ly[3] = { -0.42f * r, 0.22f * r, 0.22f * r };
      Point cp[3];
      for (int i = 0; i < 3; i++) cp[i] = rp(cx, cy, lx[i], ly[i], c, s);
      Point stem[3] = { rp(cx, cy, -0.28f * r, 0.9f * r, c, s),
                        rp(cx, cy,  0.28f * r, 0.9f * r, c, s),
                        rp(cx, cy,  0,         0.1f * r, c, s) };
      int d = max(3, (int)(rr * 2));
      if (style != 0) {
        canvas.setBrushColor(fc);
        for (int i = 0; i < 3; i++) canvas.fillEllipse(cp[i].X, cp[i].Y, d, d);
        canvas.fillPath(cp, 3);              // fills the gap between circles
        canvas.fillPath(stem, 3);
      }
      if (style != 1) {
        canvas.setPenColor(col);
        for (int i = 0; i < 3; i++) canvas.drawEllipse(cp[i].X, cp[i].Y, d, d);
        outline(stem, 3);
      }
      break;
    }
    case 14: {                               // chevron
      pts[0] = rp(cx, cy,  0,          -0.75f * r, c, s);
      pts[1] = rp(cx, cy,  0.9f * r,    0.25f * r, c, s);
      pts[2] = rp(cx, cy,  0.9f * r,    0.75f * r, c, s);
      pts[3] = rp(cx, cy,  0,          -0.25f * r, c, s);
      pts[4] = rp(cx, cy, -0.9f * r,    0.75f * r, c, s);
      pts[5] = rp(cx, cy, -0.9f * r,    0.25f * r, c, s);
      pts[6] = rp(cx, cy,  0,          -0.5f * r, c, s);     // fan centre
      drawPoly(pts, 6, 1, style, col, dim);
      break;
    }
    case 15: {                               // ring
      const int N = 14;
      float ri = r * 0.55f;
      if (style != 0) {
        canvas.setBrushColor(fc);
        for (int k = 0; k < N; k++) {
          float a0 = k * TWO_PI / N, a1 = (k + 1) * TWO_PI / N;
          Point quad[4] = {
            Point((int)(cx + cosf(a0) * r),  (int)(cy + sinf(a0) * r)),
            Point((int)(cx + cosf(a1) * r),  (int)(cy + sinf(a1) * r)),
            Point((int)(cx + cosf(a1) * ri), (int)(cy + sinf(a1) * ri)),
            Point((int)(cx + cosf(a0) * ri), (int)(cy + sinf(a0) * ri)) };
          canvas.fillPath(quad, 4);
        }
      }
      if (style != 1) {
        canvas.setPenColor(col);
        canvas.drawEllipse((int)cx, (int)cy, max(3, (int)(r * 2)),  max(3, (int)(r * 2)));
        canvas.drawEllipse((int)cx, (int)cy, max(3, (int)(ri * 2)), max(3, (int)(ri * 2)));
      }
      break;
    }
    case 16: {                               // gear: 6 teeth + centre hole
      Point g[24];
      const float da[4] = { -0.30f, -0.17f, 0.17f, 0.30f };
      const float dr[4] = {  0.72f,  1.0f,  1.0f,  0.72f };
      for (int k = 0; k < 6; k++)
        for (int j = 0; j < 4; j++) {
          float aa = rot + k * (TWO_PI / 6) + da[j];
          g[k * 4 + j] = Point((int)(cx + cosf(aa) * r * dr[j]),
                               (int)(cy + sinf(aa) * r * dr[j]));
        }
      if (style != 0) {
        Point body[12];
        ngon(body, 12, cx, cy, r * 0.73f, rot);
        fillP(body, 12, style, col, dim);
        for (int k = 0; k < 6; k++) fillP(&g[k * 4], 4, style, col, dim);
      }
      strokeP(g, 24, style, col);
      hole(cx, cy, r * 0.28f, style, col);
      break;
    }
    case 17: {                               // crown
      Point o[7] = { PT(-0.9f, 0.6f), PT(0.9f, 0.6f), PT(0.9f, -0.6f), PT(0.45f, 0.1f),
                     PT(0.0f, -0.8f), PT(-0.45f, 0.1f), PT(-0.9f, -0.6f) };
      if (style != 0) {
        Point base[4] = { PT(-0.9f, 0.1f), PT(0.9f, 0.1f), PT(0.9f, 0.6f), PT(-0.9f, 0.6f) };
        Point t1[3] = { PT(-0.9f, -0.6f), PT(-0.45f, 0.1f), PT(-0.9f, 0.1f) };
        Point t2[3] = { PT(-0.45f, 0.1f), PT(0.0f, -0.8f), PT(0.45f, 0.1f) };
        Point t3[3] = { PT(0.45f, 0.1f), PT(0.9f, -0.6f), PT(0.9f, 0.1f) };
        fillP(base, 4, style, col, dim);
        fillP(t1, 3, style, col, dim);
        fillP(t2, 3, style, col, dim);
        fillP(t3, 3, style, col, dim);
      }
      strokeP(o, 7, style, col);
      break;
    }
    case 18: {                               // shield
      Point o[8] = { PT(-0.85f, -0.8f), PT(0.0f, -0.95f), PT(0.85f, -0.8f), PT(0.85f, -0.1f),
                     PT(0.6f, 0.45f), PT(0.0f, 0.95f), PT(-0.6f, 0.45f), PT(-0.85f, -0.1f) };
      drawPoly(o, 8, 0, style, col, dim);
      break;
    }
    case 19: {                               // hourglass
      Point t1[3] = { PT(-0.7f, -0.9f), PT(0.7f, -0.9f), PT(0.0f, 0.0f) };
      Point t2[3] = { PT(0.0f, 0.0f), PT(0.7f, 0.9f), PT(-0.7f, 0.9f) };
      Point cap1[4] = { PT(-0.85f, -1.0f), PT(0.85f, -1.0f), PT(0.85f, -0.88f), PT(-0.85f, -0.88f) };
      Point cap2[4] = { PT(-0.85f, 0.88f), PT(0.85f, 0.88f), PT(0.85f, 1.0f), PT(-0.85f, 1.0f) };
      drawPoly(t1, 3, 0, style, col, dim);
      drawPoly(t2, 3, 0, style, col, dim);
      drawPoly(cap1, 4, 0, style, col, dim);
      drawPoly(cap2, 4, 0, style, col, dim);
      break;
    }
    case 20: {                               // lightning bolt
      Point o[6] = { PT(0.25f, -1.0f), PT(-0.55f, 0.15f), PT(-0.1f, 0.15f),
                     PT(-0.3f, 1.0f), PT(0.55f, -0.25f), PT(0.1f, -0.25f) };
      if (style != 0) {
        Point ta[3] = { o[0], o[1], o[5] };
        Point tb[3] = { o[1], o[2], o[5] };
        Point tc[3] = { o[2], o[5], o[4] };
        Point td[3] = { o[2], o[3], o[4] };
        fillP(ta, 3, style, col, dim);
        fillP(tb, 3, style, col, dim);
        fillP(tc, 3, style, col, dim);
        fillP(td, 3, style, col, dim);
      }
      strokeP(o, 6, style, col);
      break;
    }
    case 21: {                               // arrow (points along local +x)
      Point o[7] = { PT(-0.9f, -0.22f), PT(0.2f, -0.22f), PT(0.2f, -0.65f), PT(0.95f, 0.0f),
                     PT(0.2f, 0.65f), PT(0.2f, 0.22f), PT(-0.9f, 0.22f) };
      if (style != 0) {
        Point shaft[4] = { o[0], o[1], o[5], o[6] };
        Point head[3]  = { o[2], o[3], o[4] };
        fillP(shaft, 4, style, col, dim);
        fillP(head, 3, style, col, dim);
      }
      strokeP(o, 7, style, col);
      break;
    }
    case 22: {                               // flower: 6 petals + centre
      float pr = r * 0.36f, pd = r * 0.62f;
      int d = max(3, (int)(pr * 2));
      for (int k = 0; k < 6; k++) {
        float a = rot + k * TWO_PI / 6;
        int ppx = (int)(cx + cosf(a) * pd), ppy = (int)(cy + sinf(a) * pd);
        if (style != 0) { canvas.setBrushColor(fc);  canvas.fillEllipse(ppx, ppy, d, d); }
        if (style != 1) { canvas.setPenColor(col);   canvas.drawEllipse(ppx, ppy, d, d); }
      }
      hole(cx, cy, r * 0.2f, style, col);
      break;
    }
    case 23: {                               // bell
      Point b[14] = { PT(0.0f, -0.85f), PT(0.25f, -0.78f), PT(0.42f, -0.58f), PT(0.48f, -0.25f),
                      PT(0.52f, 0.15f), PT(0.68f, 0.45f), PT(0.9f, 0.62f),
                      PT(-0.9f, 0.62f), PT(-0.68f, 0.45f), PT(-0.52f, 0.15f), PT(-0.48f, -0.25f),
                      PT(-0.42f, -0.58f), PT(-0.25f, -0.78f) };
      b[13] = PT(0.0f, 0.1f);                // fan centre
      drawPoly(b, 13, 1, style, col, dim);
      Point cl = PT(0.0f, 0.86f);            // clapper
      int d = max(3, (int)(r * 0.3f));
      if (style != 0) { canvas.setBrushColor(fc);  canvas.fillEllipse(cl.X, cl.Y, d, d); }
      if (style != 1) { canvas.setPenColor(col);   canvas.drawEllipse(cl.X, cl.Y, d, d); }
      break;
    }
    case 24: {                               // music note (eighth note)
      Point head[10];
      float tc = cosf(-0.4f), ts = sinf(-0.4f);
      for (int i = 0; i < 10; i++) {
        float t  = i * TWO_PI / 10;
        float ex = cosf(t) * 0.36f, ey = sinf(t) * 0.24f;
        head[i] = PT(-0.34f + ex * tc - ey * ts, 0.62f + ex * ts + ey * tc);
      }
      Point stem[4] = { PT(-0.10f, -0.9f), PT(0.02f, -0.9f), PT(0.02f, 0.62f), PT(-0.10f, 0.62f) };
      Point flag[4] = { PT(0.02f, -0.9f), PT(0.60f, -0.5f), PT(0.68f, -0.05f), PT(0.02f, -0.5f) };
      drawPoly(head, 10, 0, style, col, dim);
      drawPoly(stem, 4, 0, style, col, dim);
      drawPoly(flag, 4, 0, style, col, dim);
      break;
    }
    case 25: {                               // wrench (open end points along local +x)
      Point o[16];
      int n = 0;
      o[n++] = PT(-0.95f, -0.14f);
      for (int i = 0; i < 6; i++) {
        float a = -2.846f + i * (2.484f / 5);
        o[n++] = PT(0.5f + 0.48f * cosf(a), 0.48f * sinf(a));
      }
      o[n++] = PT(0.45f, -0.17f);
      o[n++] = PT(0.45f, 0.17f);
      for (int i = 0; i < 6; i++) {
        float a = 0.362f + i * (2.484f / 5);
        o[n++] = PT(0.5f + 0.48f * cosf(a), 0.48f * sinf(a));
      }
      o[n++] = PT(-0.95f, 0.14f);
      if (style != 0) {
        Point handle[4] = { PT(-0.95f, -0.14f), PT(0.2f, -0.14f), PT(0.2f, 0.14f), PT(-0.95f, 0.14f) };
        Point head[12];
        for (int i = 0; i < 12; i++) {
          float a = i * TWO_PI / 12;
          head[i] = PT(0.5f + 0.48f * cosf(a), 0.48f * sinf(a));
        }
        fillP(handle, 4, style, col, dim);
        fillP(head, 12, style, col, dim);
        Point slot[4] = { PT(0.45f, -0.17f), PT(1.1f, -0.17f), PT(1.1f, 0.17f), PT(0.45f, 0.17f) };
        canvas.setBrushColor(bgCol);         // cut the jaw opening
        canvas.fillPath(slot, 4);
      }
      strokeP(o, n, style, col);
      break;
    }
    case 26: {                               // envelope
      Point e[4] = { PT(-0.95f, -0.62f), PT(0.95f, -0.62f), PT(0.95f, 0.62f), PT(-0.95f, 0.62f) };
      drawPoly(e, 4, 0, style, col, dim);
      Point mid = PT(0.0f, 0.12f);
      canvas.setPenColor(style == 1 ? bgCol : col);
      canvas.drawLine(e[0].X, e[0].Y, mid.X, mid.Y);
      canvas.drawLine(mid.X, mid.Y, e[1].X, e[1].Y);
      break;
    }
    case 27: {                               // acid smiley
      int d = max(3, (int)(r * 2));
      if (style != 0) { canvas.setBrushColor(fc);  canvas.fillEllipse((int)cx, (int)cy, d, d); }
      if (style != 1) { canvas.setPenColor(col);   canvas.drawEllipse((int)cx, (int)cy, d, d); }
      Point e1 = PT(-0.35f, -0.28f), e2 = PT(0.35f, -0.28f);
      hole((float)e1.X, (float)e1.Y, r * 0.14f, style, col);
      hole((float)e2.X, (float)e2.Y, r * 0.14f, style, col);
      Point m[9];
      for (int i = 0; i < 9; i++) {
        float t = 0.35f + i * (PI - 0.7f) / 8;
        m[i] = PT(0.58f * cosf(t), 0.08f + 0.5f * sinf(t));
      }
      float th = max(1.5f, r * 0.12f);
      for (int i = 0; i < 8; i++) thickSeg(m[i], m[i + 1], th, style, bgCol, col);
      break;
    }
    case 28: {                               // infinity
      float th = max(1.5f, r * 0.17f);
      Point prev = PT(lemX[0], lemY[0]);
      for (int i = 1; i <= LEM_N; i++) {
        Point cur = PT(lemX[i % LEM_N], lemY[i % LEM_N]);
        thickSeg(prev, cur, th, style, fc, col);
        prev = cur;
      }
      break;
    }
    case 29: {                               // dollar sign
      static const float sx[13] = { 0.40f, 0.28f, 0.0f, -0.28f, -0.40f, -0.28f, 0.0f,
                                    0.28f, 0.40f, 0.28f, 0.0f, -0.28f, -0.40f };
      static const float sy[13] = { -0.50f, -0.72f, -0.80f, -0.72f, -0.50f, -0.28f, 0.0f,
                                     0.28f, 0.50f, 0.72f, 0.80f, 0.72f, 0.50f };
      float th = max(1.5f, r * 0.15f);
      for (int i = 0; i < 12; i++)
        thickSeg(PT(sx[i], sy[i]), PT(sx[i + 1], sy[i + 1]), th, style, fc, col);
      thickSeg(PT(0.0f, -0.98f), PT(0.0f, 0.98f), th * 0.8f, style, fc, col);
      break;
    }
    case 30: {                               // euro sign
      Point cp[9];
      for (int i = 0; i < 9; i++) {
        float a = 0.75f + i * (TWO_PI - 1.5f) / 8;
        cp[i] = PT(0.1f + 0.65f * cosf(a), 0.8f * sinf(a));
      }
      float th = max(1.5f, r * 0.15f);
      for (int i = 0; i < 8; i++) thickSeg(cp[i], cp[i + 1], th, style, fc, col);
      thickSeg(PT(-0.75f, -0.18f), PT(0.35f, -0.18f), th * 0.8f, style, fc, col);
      thickSeg(PT(-0.75f,  0.18f), PT(0.35f,  0.18f), th * 0.8f, style, fc, col);
      break;
    }
    default: {                               // 31: yen sign
      float th = max(1.5f, r * 0.15f);
      thickSeg(PT(-0.5f, -0.9f), PT(0.0f, 0.0f), th, style, fc, col);
      thickSeg(PT( 0.5f, -0.9f), PT(0.0f, 0.0f), th, style, fc, col);
      thickSeg(PT(0.0f, 0.0f), PT(0.0f, 0.9f), th, style, fc, col);
      thickSeg(PT(-0.42f, 0.10f), PT(0.42f, 0.10f), th * 0.8f, style, fc, col);
      thickSeg(PT(-0.42f, 0.42f), PT(0.42f, 0.42f), th * 0.8f, style, fc, col);
      break;
    }
  }
}

// ---------- Word layer (MIDI channel 9) ----------
// CC20 value 0..127 = word number
static const char* const WORDS[] = {
  "ACID", "DANCE", "FREEDOM", "FUTURE", "MUSIC", "HOUSE", "CASH", "MONEY",       //   0-7
  "POWER", "XTC", "FREAK", "LIVE", "NASTY", "SEX", "RAVE", "TECHNO",             //   8-15
  "BASS", "BEAT", "GROOVE", "FUNK", "DISCO", "TRANCE", "DRUM", "RHYTHM",         //  16-23
  "SOUL", "VIBE", "PARTY", "NIGHT", "CLUB", "DJ", "MIX", "LOOP",                 //  24-31
  "PULSE", "WAVE", "LOVE", "PEACE", "UNITY", "JOY", "TRIP", "DREAM",             //  32-39
  "HIGH", "SMILE", "ENERGY", "FIRE", "LASER", "STROBE", "NEON", "LIGHT",         //  40-47
  "DARK", "SHADOW", "GLITCH", "ROBOT", "CYBER", "DIGITAL", "SYNTH", "ANALOG",    //  48-55
  "SAMPLE", "ECHO", "REVERB", "DELAY", "FILTER", "BASSLINE", "KICK", "SNARE",    //  56-63
  "HAT", "CLAP", "TEMPO", "BPM", "DROP", "BUILD", "BREAK", "RISE",               //  64-71
  "FALL", "FLOW", "GOLD", "DIAMOND", "LUXURY", "RICH", "GREED", "BANK",          //  72-79
  "DOLLAR", "EURO", "YEN", "PROFIT", "CRAZY", "WILD", "DIRTY", "DEEP",           //  80-87
  "HARD", "FAST", "LOUD", "RAW", "SICK", "FRESH", "UNDERGROUND", "WAREHOUSE",    //  88-95
  "CHICAGO", "DETROIT", "BERLIN", "IBIZA", "LONDON", "TOKYO", "NEW YORK", "SUN", //  96-103
  "MOON", "STAR", "SPACE", "COSMIC", "GALAXY", "ALIEN", "ROCKET", "PLANET",      // 104-111
  "ORBIT", "HYPNOTIC", "EUPHORIA", "BLISS", "RUSH", "ECSTASY", "SMILEY", "GLOW", // 112-119
  "SPARK", "STORM", "THUNDER", "REBEL", "RIOT", "SYSTEM", "REPEAT", "ERROR"      // 120-127
};
static_assert(sizeof(WORDS) / sizeof(WORDS[0]) == NUM_WORDS, "WORDS must hold exactly 128 entries");

// font steps picked from the shape size (text can't be scaled or rotated)
const fabgl::FontInfo* wordFonts[4] = { &fabgl::FONT_8x8, &fabgl::FONT_std_12,
                                        &fabgl::FONT_std_16, &fabgl::FONT_std_24 };
const int wordH[4] = { 8, 12, 16, 24 };

// style 0 = plain text, 1 = solid box with inverted text, 2 = dim box + bright text
void drawWord(int idx, float cx, float cy, float r, int style, RGB888 col, RGB888 dim) {
  const char* w = WORDS[idx & (NUM_WORDS - 1)];
  int sz = (r < 14) ? 0 : (r < 26) ? 1 : (r < 46) ? 2 : 3;
  canvas.selectFont(wordFonts[sz]);
  int tw = canvas.textExtent(w);
  int th = wordH[sz];
  int x = (int)cx - tw / 2, y = (int)cy - th / 2;
  if (style != 0) {
    canvas.setBrushColor(style == 2 ? dim : col);
    canvas.fillRectangle(x - 2, y - 1, x + tw + 1, y + th);
  }
  canvas.setPenColor(style == 1 ? bgCol : col);
  canvas.drawText(x, y, w);
}

// ---------- MIDI handlers (called from loop) ----------
void onNoteOn(byte ch, byte note, byte vel) {
  if (ch < 1 || ch > NUM_CH) return;
  Chan& c = chans[ch - 1];
  if (vel == 0) {                            // note-on w/ vel 0 = note off
    for (auto& v : c.v) if (v.active && v.held && v.note == note) v.held = false;
    return;
  }
  Voice* slot = nullptr;
  for (auto& v : c.v) if (v.active && v.note == note) { slot = &v; break; }   // retrigger
  if (!slot) for (auto& v : c.v) if (!v.active) { slot = &v; break; }         // free
  if (!slot) {                                                                // steal quietest
    slot = &c.v[0];
    for (auto& v : c.v) if (v.life < slot->life) slot = &v;
  }
  slot->active = true;  slot->held = true;
  slot->note = note;    slot->vel = vel / 127.0f;
  slot->life = 1.0f;    slot->age = 0;
}

void onNoteOff(byte ch, byte note, byte vel) {
  if (ch < 1 || ch > NUM_CH) return;
  for (auto& v : chans[ch - 1].v) if (v.active && v.held && v.note == note) v.held = false;
}

void onCC(byte ch, byte cc, byte val) {
#if DEBUG_CC
  Serial.printf("CC ch%d cc%d = %d\n", ch, cc, val);
#endif
  if (ch == BG_CHANNEL) {                    // background + preset buttons
    switch (cc) {
      case 20: bgHue = val; break;
      case 21: bgSat = val; break;
      case 22: bgVal = val; break;
      case CC_LOAD: if (val >= 64) loadSlot(curSlot); break;
      case CC_SAVE: if (val >= 64) saveSlot(curSlot); break;
      case CC_SLOTBAR: slotBarEnabled = (val >= 64); break;
      case 120: case 123:                    // all notes off on ch16 = every layer
        for (auto& c : chans) for (auto& v : c.v) v.held = false;
        break;
    }
    return;
  }
  if (ch < 1 || ch > NUM_CH) return;
  Chan& c = chans[ch - 1];
  if (cc >= CC_BASE && cc < CC_BASE + NUM_CC_BLK) {
    c.cc[cc - CC_BASE] = val;
  } else if (cc == CC_ORBMODE) {
    c.cc[P_ORBMODE] = val;
  } else if (cc == CC_ORBSPD) {
    c.cc[P_ORBSPD] = val;
  } else if (cc == 120 || cc == 123) {       // all sound/notes off
    for (auto& v : c.v) v.held = false;
  }
}

void onProgram(byte ch, byte prog) {         // any channel selects the preset slot
#if DEBUG_CC
  Serial.printf("PC ch%d prog %d\n", ch, prog);
#endif
  if (prog < NUM_SLOTS) {
    curSlot = prog;
    showSlots(0);
  }
}

void onPitchBend(byte ch, int bend) {        // -8192..8191 -> rotation offset
  if (ch < 1 || ch > NUM_CH) return;
  chans[ch - 1].bend = (bend / 8192.0f) * PI;
}

void processMidi() {
  MidiMsg m;
  for (int n = 0; n < 64 && xQueueReceive(midiQ, &m, 0) == pdTRUE; n++) {
    uint8_t ch = (m.status & 0x0F) + 1;
    switch (m.status & 0xF0) {
      case 0x90: onNoteOn(ch, m.d1, m.d2);  break;
      case 0x80: onNoteOff(ch, m.d1, m.d2); break;
      case 0xB0: onCC(ch, m.d1, m.d2);      break;
      case 0xC0: onProgram(ch, m.d1);       break;
      case 0xE0: onPitchBend(ch, (int)((m.d2 << 7) | m.d1) - 8192); break;
    }
  }
}

// ---------- Mode handling ----------
void releaseAll() {
  for (auto& c : chans) for (auto& v : c.v) v.held = false;
}

void applyMode() {
  releaseAll();
  if (mode == MODE_DEVICE) {
    NimBLEDevice::getScan()->stop();
    if (client && client->isConnected()) client->disconnect();
    if (haveTarget) { delete targetAddr; targetAddr = nullptr; haveTarget = false; }
    if (!adv->isAdvertising()) adv->start();
  } else {
    adv->stop();
    for (auto id : bleServer->getPeerDevices()) bleServer->disconnect(id);
  }
}

void toggleMode() {
  mode = (mode == MODE_DEVICE) ? MODE_KEYBOARD : MODE_DEVICE;
  prefs.putUChar("mode", (uint8_t)mode);
  applyMode();
}

// Connect to the keyboard found by the scanner (blocks for ~1-5 s)
void connectKeyboard() {
  NimBLEAddress addr = *targetAddr;
  delete targetAddr; targetAddr = nullptr; haveTarget = false;

  if (!client) {
    client = NimBLEDevice::createClient();
    client->setConnectTimeout(5);
  }
  if (!client->connect(addr)) return;        // scan restarts automatically

  NimBLERemoteService* svc = client->getService(MIDI_SVC);
  NimBLERemoteCharacteristic* chr = svc ? svc->getCharacteristic(MIDI_CHR) : nullptr;
  if (!chr || !chr->canNotify() || !chr->subscribe(true, notifyCB)) {
    client->disconnect();
  }
}

void keyboardTask() {
  if (client && client->isConnected()) return;
  if (haveTarget) { connectKeyboard(); return; }

  static uint32_t lastScan = 0;
  NimBLEScan* sc = NimBLEDevice::getScan();
  if (!sc->isScanning() && millis() - lastScan > 300) {
    lastScan = millis();
    sc->start(10, nullptr, false);           // 10 s windows, results cleared each time
  }
}

void deviceTask() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  if (bleServer->getConnectedCount() == 0 && !adv->isAdvertising()) adv->start();
}

// ---------- Button (IO36) ----------
int btnIdle = HIGH;

void buttonTask() {
  static bool lastPressed = false;
  static uint32_t lastChange = 0;
  bool pressed = (digitalRead(BTN_PIN) != btnIdle);
  if (pressed != lastPressed && millis() - lastChange > 40) {
    lastChange = millis();
    lastPressed = pressed;
    if (pressed) toggleMode();
  }
}

// ---------- Update + draw ----------
void update(float dt) {
  for (auto& c : chans) {
    float spinSpeed = ((int)c.cc[P_SPIN]   - 64) / 64.0f * 4.0f;   // rad/s, 64 = stopped
    float orbSpeed  = ((int)c.cc[P_ORBSPD] - 64) / 64.0f * 4.0f;
    c.spin     += spinSpeed * dt;
    c.orbAngle += orbSpeed  * dt;
    if (c.spin     > TWO_PI * 100 || c.spin     < -TWO_PI * 100) c.spin     = fmodf(c.spin,     TWO_PI);
    if (c.orbAngle > TWO_PI * 100 || c.orbAngle < -TWO_PI * 100) c.orbAngle = fmodf(c.orbAngle, TWO_PI);

    float rel = (c.cc[P_RELEASE] / 127.0f);
    float relTime = 0.1f + rel * rel * 4.0f;                   // 0.1 .. 4.1 s
    for (auto& v : c.v) {
      if (!v.active) continue;
      v.age += dt;
      if (!v.held) v.life -= dt / relTime;
      if (v.life <= 0) v.active = false;
    }
  }
}

void render() {
  bgCol = hsv(bgHue / 127.0f, bgSat / 127.0f, bgVal / 127.0f);
  canvas.setBrushColor(bgCol);
  canvas.clear();
  canvas.setPenWidth(1);

  for (int ci = 0; ci < NUM_CH; ci++) {
    Chan& c = chans[ci];
    int   type    = (c.cc[P_SHAPE] * NUM_SHAPES) >> 7;         // 0..31
    int   style   = (c.cc[P_STYLE] * 3) >> 7;                  // 0..2
    int   copies  = 1 + ((c.cc[P_COPIES] * 12) >> 7);          // 1..12
    int   omode   = (c.cc[P_ORBMODE] * 3) >> 7;                // 0 pitch, 1 auto, 2 both
    float baseR   = 6 + (c.cc[P_SIZE] / 127.0f) * 50;
    float orbit   = (c.cc[P_ORBIT] / 127.0f) * 90;
    float spread  = c.cc[P_SPREAD] / 127.0f;
    float expand  = c.cc[P_EXPAND] / 127.0f;
    float hueBase = c.cc[P_HUE] / 127.0f;
    float ax      = W * (c.cc[P_X] / 127.0f);
    float ay      = H * (c.cc[P_Y] / 127.0f);

    for (auto& v : c.v) {
      if (!v.active) continue;
      float pc   = (v.note % 12) / 12.0f;
      float r    = baseR * (0.4f + 0.6f * v.vel) * (1.0f + expand * v.age * 0.8f);
      if (r > 110) r = 110;
      float bright = 0.3f + 0.7f * v.life;

      float posBase = (omode != 1 ? pc * TWO_PI : 0.0f)
                    + (omode != 0 ? c.orbAngle   : 0.0f)
                    + c.bend;

      for (int i = 0; i < copies; i++) {
        // hue spread shifts by pitch class AND by copy index
        float hv = hueBase + spread * (pc + (float)i / copies);
        RGB888 col = hsv(hv, 1.0f, bright);
        RGB888 dim = hsv(hv, 1.0f, bright * 0.45f);

        float a  = posBase + i * TWO_PI / copies;
        float px = ax + cosf(a) * orbit;
        float py = ay + sinf(a) * orbit;
        if (ci == WORD_CH) drawWord(c.cc[P_SHAPE], px, py, r, style, col, dim);
        else               drawShape(type, px, py, r, a + c.spin + v.note * 0.1f, style, col, dim);
      }
    }
  }

  drawSlotBar();

  // not-connected dot (top-left): red = DEVICE mode, yellow = KEYBOARD mode
  // nothing is drawn once connected
  if (!isConnected()) {
    canvas.setBrushColor(mode == MODE_DEVICE ? RGB888(255, 0, 0) : RGB888(255, 255, 0));
    canvas.fillEllipse(8, 8, 8, 8);
  }

#if DOUBLE_BUFFER
  canvas.swapBuffers();
#endif
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_CH; i++) memcpy(chans[i].cc, DEFAULTS[i], NUM_CC);
  initTables();

  // button: detect idle level (GPIO36 has no internal pull-up/down)
  pinMode(BTN_PIN, INPUT);
  delay(20);
  int highs = 0;
  for (int i = 0; i < 16; i++) { highs += digitalRead(BTN_PIN); delay(2); }
  btnIdle = (highs >= 8) ? HIGH : LOW;

  prefs.begin("viz", false);
  mode = (prefs.getUChar("mode", MODE_DEVICE) == MODE_KEYBOARD) ? MODE_KEYBOARD : MODE_DEVICE;

  pre.begin("presets", false);
  slotUsed = pre.getUInt("used", 0);

  midiQ = xQueueCreate(128, sizeof(MidiMsg));

  DisplayController.begin();   // default pins match the TTGO VGA32
  DisplayController.setResolution(QVGA_320x240_60Hz, W, H, DOUBLE_BUFFER);

  // ---- BLE setup (server for DEVICE mode, scanner/client for KEYBOARD mode) ----
  NimBLEDevice::init("VGA-MIDI-Viz");

  bleServer = NimBLEDevice::createServer();
  bleServer->advertiseOnDisconnect(false);   // we restart advertising ourselves
  NimBLEService* svc = bleServer->createService(MIDI_SVC);
  NimBLECharacteristic* chr = svc->createCharacteristic(
      MIDI_CHR,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR |
      NIMBLE_PROPERTY::NOTIFY);
  chr->setCallbacks(&chrCB);
  svc->start();

  adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(MIDI_SVC);
  adv->setScanResponse(true);

  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setAdvertisedDeviceCallbacks(&scanCB, false);
  sc->setActiveScan(true);
  sc->setInterval(97);
  sc->setWindow(37);

  applyMode();
}

void loop() {
  static uint32_t last = micros();
  uint32_t now = micros();
  float dt = (now - last) / 1e6f;
  last = now;

  buttonTask();
  if (mode == MODE_KEYBOARD) keyboardTask(); else deviceTask();

  processMidi();
  update(dt);
  render();
}
