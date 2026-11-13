// =====================================================================
//  SMART DESK COMPANION  (ESP32-S3 + 2.8" ST7789 + XPT2046 + LVGL 8.3)
//  Pink theme | Boot animation | WiFi + NTP clock | Weather | Pomodoro | Switches
// =====================================================================
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>      // ArduinoJson 7.x
#include <time.h>
#include <lvgl.h>             // LVGL 8.3.x
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <XPT2046_Touchscreen.h>

// Forward declaration: Arduino auto-generated function prototypes use SwTile.
struct SwTile;

// ------------------------- USER SETTINGS -----------------------------
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASS      "YOUR_WIFI_PASSWORD"

#define LOCATION_NAME  "Place, City"
#define WX_LAT         21.10      // apna exact lat/lon Google Maps se daal lena
#define WX_LON         79.00

#define GMT_OFFSET_SEC 19800      // India = UTC+5:30

// Output pins (apne hisab se badal sakte ho)
#define PIN_BUZZER     4          // active buzzer (+ pin)
#define PIN_DESK_LIGHT 8
#define PIN_FAN        9
#define PIN_NIGHT_LAMP 10
#define RELAY_ON       HIGH       // active-low relay module ho toh LOW kar do
#define RELAY_OFF      LOW

// ------------------------- DISPLAY PINS ------------------------------
#define TFT_CS      5
#define TFT_DC      7
#define TFT_RST     6
#define SPI_SCK     16
#define SPI_MOSI    15
#define SPI_MISO    18
#define TOUCH_CS    17

#define TFT_ROTATION  1
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

// Touch calibration (raw values). Galat jagah touch ho toh adjust karo.
#define TS_MIN_X 200
#define TS_MAX_X 3800
#define TS_MIN_Y 200
#define TS_MAX_Y 3800
#define TS_FLIP_X 1     // swipe/touch left-right ulta ho toh 0/1 badlo
#define TS_FLIP_Y 0     // upar-neeche ulta ho toh 1 karo

// ------------------------- PINK THEME --------------------------------
#define C_BG          0xFFF0F6
#define C_CARD        0xFFFFFF
#define C_PINK        0xEC4899
#define C_PINK_DARK   0xBE185D
#define C_PINK_LIGHT  0xFBCFE8
#define C_TEXT        0x4A1D3A
#define C_MUTED       0x9D6B86
#define C_ORANGE      0xFF8A00

// Pomodoro "ocean" theme (pomo_bg.c wali image ke colours se match)
#define OC_NAVY       0x0F3A5F
#define OC_DEEP       0x0B2E4A
#define OC_AQUA       0x5CE1E6
#define OC_BLUE       0x2F7BFF
#define OC_VIOLET     0xA78BFA
#define POMO_GROUND   0x002842      // image ke neeche ka ground colour (tab ke bache hue 22px)

#define USE_POMO_ANIM 1           // 1 = animated background (pomo_anim.c), sketch folder me honi chahiye
#define USE_POMO_BG   0           // 1 = static image (pomo_bg.c); sirf tab chalta hai jab USE_POMO_ANIM 0 ho
#if USE_POMO_ANIM
extern const lv_img_dsc_t * const pomo_anim_frames[];
extern const int pomo_anim_count;
extern const int pomo_anim_ms;
#elif USE_POMO_BG
LV_IMG_DECLARE(pomo_bg);
#endif

// ------------------------- OBJECTS -----------------------------------
Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
XPT2046_Touchscreen ts(TOUCH_CS);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[SCREEN_WIDTH * 20];
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// UI handles
static lv_obj_t *date_label, *wifi_label, *ampm_label, *wd_label[7];
static void clock_update(const struct tm *ti, bool ok);

// Flip clock digit (top pe, taaki Arduino ke auto-prototype me dikkat na aaye)
struct Digit {
  lv_obj_t *cont;
  lv_obj_t *seg[7];
  lv_obj_t *cover;
  int cur;
  int pending;
  bool animating;
};
static Digit digits[4];
static lv_obj_t *temp_label, *cond_label, *hum_label;
static lv_obj_t *pomo_arc, *pomo_time, *pomo_start_label;

// Pomodoro state
#define POMO_TOTAL (25 * 60)
static bool     pomo_running = false;
static uint32_t time_left = POMO_TOTAL;

// Weather state (weather task likhta hai, UI padhta hai)
static volatile bool  wx_valid = false;
static volatile float wx_temp = 0;
static volatile int   wx_hum = 0;
static volatile int   wx_code = 0;

// =====================================================================
//  LVGL <-> DISPLAY / TOUCH
// =====================================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((uint16_t *)color_p, w * h, true, false);
  tft.endWrite();
  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev, lv_indev_data_t *data) {
  digitalWrite(TFT_CS, HIGH);
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    int16_t x = map(p.x, TS_MIN_X, TS_MAX_X, TS_FLIP_X ? SCREEN_WIDTH - 1 : 0, TS_FLIP_X ? 0 : SCREEN_WIDTH - 1);
    int16_t y = map(p.y, TS_MIN_Y, TS_MAX_Y, TS_FLIP_Y ? SCREEN_HEIGHT - 1 : 0, TS_FLIP_Y ? 0 : SCREEN_HEIGHT - 1);
    x = constrain(x, 0, SCREEN_WIDTH - 1);
    y = constrain(y, 0, SCREEN_HEIGHT - 1);
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// =====================================================================
//  HELPERS
// =====================================================================
static void beep(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(150);
    digitalWrite(PIN_BUZZER, LOW);
    delay(120);
  }
}

static const char *weather_text(int c) {
  if (c == 0) return "Clear Sky";
  if (c <= 2) return "Partly Cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Foggy";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rainy";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c >= 95) return "Thunderstorm";
  return "--";
}

static lv_obj_t *make_card(lv_obj_t *parent, int w, int h) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(c, 16, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_shadow_width(c, 10, 0);
  lv_obj_set_style_shadow_color(c, lv_color_hex(C_PINK), 0);
  lv_obj_set_style_shadow_opa(c, LV_OPA_30, 0);
  lv_obj_set_style_shadow_ofs_y(c, 2, 0);
  lv_obj_set_style_pad_all(c, 8, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

// =====================================================================
//  WEATHER TASK (alag core pe, UI freeze nahi hoga)
// =====================================================================
void weather_task(void *arg) {
  for (;;) {
    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(WX_LAT, 4) +
                   "&longitude=" + String(WX_LON, 4) +
                   "&current=temperature_2m,relative_humidity_2m,weather_code&timezone=Asia%2FKolkata";
      http.setTimeout(8000);
      if (http.begin(client, url)) {
        if (http.GET() == 200) {
          DynamicJsonDocument doc(1024);
          if (!deserializeJson(doc, http.getString())) {
            wx_temp = doc["current"]["temperature_2m"] | 0.0f;
            wx_hum  = doc["current"]["relative_humidity_2m"] | 0;
            wx_code = doc["current"]["weather_code"] | 0;
            wx_valid = true;
            ok = true;
          }
        }
        http.end();
      }
    }
    vTaskDelay((ok ? 600000 : 30000) / portTICK_PERIOD_MS);   // 10 min / retry 30 sec
  }
}

// =====================================================================
//  UI UPDATE (har 1 second)
// =====================================================================
void ui_tick_cb(lv_timer_t *t) {
  struct tm ti;
  bool tok = getLocalTime(&ti, 0);
  clock_update(&ti, tok);

  bool wifi_ok = (WiFi.status() == WL_CONNECTED);
  lv_obj_set_style_text_color(wifi_label, lv_color_hex(wifi_ok ? C_ORANGE : 0x555555), 0);

  if (wx_valid) {
    lv_label_set_text_fmt(temp_label, "%d°C", (int)roundf(wx_temp));
    lv_label_set_text(cond_label, weather_text(wx_code));
    lv_label_set_text_fmt(hum_label, "Humidity %d%%", (int)wx_hum);
  }
}

// =====================================================================
//  POMODORO
// =====================================================================
static void pomo_refresh() {
  lv_label_set_text_fmt(pomo_time, "%02u:%02u", (unsigned)(time_left / 60), (unsigned)(time_left % 60));
  lv_arc_set_value(pomo_arc, time_left);
}

void pomo_timer_cb(lv_timer_t *t) {
  if (!pomo_running) return;
  if (time_left > 0) {
    time_left--;
    pomo_refresh();
  }
  if (time_left == 0) {
    pomo_running = false;
    lv_label_set_text(pomo_time, "Done!");
    lv_label_set_text(pomo_start_label, "Start");
    beep(3);
  }
}

void pomo_start_cb(lv_event_t *e) {
  if (time_left == 0) {
    time_left = POMO_TOTAL;
    pomo_refresh();
  }
  pomo_running = !pomo_running;
  lv_label_set_text(pomo_start_label, pomo_running ? "Pause" : "Start");
}

void pomo_reset_cb(lv_event_t *e) {
  pomo_running = false;
  time_left = POMO_TOTAL;
  pomo_refresh();
  lv_label_set_text(pomo_start_label, "Start");
}

// =====================================================================
//  SWITCHES  (dark neon tiles)
// =====================================================================
#define SW_BG        0x0B1220
#define SW_TILE      0x131C2E
#define SW_TILE_LINE 0x24324A
#define SW_OFF_ICON  0x24324A
#define SW_OFF_TEXT  0x7B8BA6

// Switch tile data structure (must be declared before sw_tiles and callbacks)
struct SwTile {
  uint8_t pin;
  uint32_t color;
  const char *name;
  const char *symbol;
  lv_obj_t *tile;
  lv_obj_t *icon_bg;
  lv_obj_t *icon;
  lv_obj_t *state;
  lv_obj_t *sw;
};

static SwTile sw_tiles[3] = {
  {PIN_DESK_LIGHT, 0xFBBF24, "Desk Light", LV_SYMBOL_CHARGE,   NULL, NULL, NULL, NULL, NULL},
  {PIN_FAN,        0x22D3EE, "Fan",        LV_SYMBOL_REFRESH,  NULL, NULL, NULL, NULL, NULL},
  {PIN_NIGHT_LAMP, 0xA78BFA, "Night Lamp", LV_SYMBOL_EYE_OPEN, NULL, NULL, NULL, NULL, NULL},
};

static void sw_apply(SwTile *t, bool on) {
  lv_obj_set_style_bg_color(t->icon_bg, lv_color_hex(on ? t->color : SW_OFF_ICON), 0);
  lv_obj_set_style_text_color(t->icon, lv_color_hex(on ? 0x0B1220 : SW_OFF_TEXT), 0);
  lv_label_set_text(t->state, on ? "ON" : "OFF");
  lv_obj_set_style_text_color(t->state, lv_color_hex(on ? t->color : SW_OFF_TEXT), 0);
  lv_obj_set_style_border_color(t->tile, lv_color_hex(on ? t->color : SW_TILE_LINE), 0);
  lv_obj_set_style_shadow_opa(t->tile, on ? LV_OPA_30 : LV_OPA_TRANSP, 0);
}

void switch_event_cb(lv_event_t *e) {
  SwTile *t = (SwTile *)lv_event_get_user_data(e);
  bool on = lv_obj_has_state(t->sw, LV_STATE_CHECKED);
  digitalWrite(t->pin, on ? RELAY_ON : RELAY_OFF);
  sw_apply(t, on);
  Serial.printf("GPIO %d (%s) -> %s\n", t->pin, t->name, on ? "ON" : "OFF");
}

void tile_click_cb(lv_event_t *e) {          // tile pe kahin bhi dabao toh toggle
  SwTile *t = (SwTile *)lv_event_get_user_data(e);
  if (lv_obj_has_state(t->sw, LV_STATE_CHECKED)) lv_obj_clear_state(t->sw, LV_STATE_CHECKED);
  else lv_obj_add_state(t->sw, LV_STATE_CHECKED);
  lv_event_send(t->sw, LV_EVENT_VALUE_CHANGED, NULL);
}

static void make_tile(lv_obj_t *parent, SwTile *t, int x) {
  lv_obj_t *tile = lv_obj_create(parent);
  t->tile = tile;
  lv_obj_set_pos(tile, x, 0);
  lv_obj_set_size(tile, 96, 174);
  lv_obj_set_style_bg_color(tile, lv_color_hex(SW_TILE), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 18, 0);
  lv_obj_set_style_border_width(tile, 2, 0);
  lv_obj_set_style_border_color(tile, lv_color_hex(SW_TILE_LINE), 0);
  lv_obj_set_style_shadow_width(tile, 18, 0);
  lv_obj_set_style_shadow_color(tile, lv_color_hex(t->color), 0);
  lv_obj_set_style_shadow_opa(tile, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

  t->icon_bg = lv_obj_create(tile);
  lv_obj_set_size(t->icon_bg, 58, 58);
  lv_obj_set_style_radius(t->icon_bg, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(t->icon_bg, 0, 0);
  lv_obj_set_style_shadow_width(t->icon_bg, 0, 0);
  lv_obj_set_style_pad_all(t->icon_bg, 0, 0);
  lv_obj_clear_flag(t->icon_bg, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_align(t->icon_bg, LV_ALIGN_TOP_MID, 0, 12);

  t->icon = lv_label_create(t->icon_bg);
  lv_label_set_text(t->icon, t->symbol);
  lv_obj_set_style_text_font(t->icon, &lv_font_montserrat_28, 0);
  lv_obj_center(t->icon);

  lv_obj_t *name = lv_label_create(tile);
  lv_label_set_text(name, t->name);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(0xE5EDF8), 0);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 82);

  t->state = lv_label_create(tile);
  lv_label_set_text(t->state, "OFF");
  lv_obj_align(t->state, LV_ALIGN_TOP_MID, 0, 106);

  t->sw = lv_switch_create(tile);
  lv_obj_set_size(t->sw, 56, 28);
  lv_obj_set_style_bg_color(t->sw, lv_color_hex(SW_OFF_ICON), LV_PART_MAIN);
  lv_obj_set_style_bg_color(t->sw, lv_color_hex(t->color), LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(t->sw, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_align(t->sw, LV_ALIGN_BOTTOM_MID, 0, -14);

  lv_obj_add_event_cb(t->sw, switch_event_cb, LV_EVENT_VALUE_CHANGED, t);
  lv_obj_add_event_cb(tile, tile_click_cb, LV_EVENT_CLICKED, t);
  sw_apply(t, false);
}

// =====================================================================
//  TABS
// =====================================================================
// ---------- FLIP CLOCK: 7-segment, white on black, orange accents ----------
#define DIG_W 36
#define DIG_H 100
#define DIG_T 8
#define DIG_Y 30


// bit0=a(top) b c d e f g(mid); index 10 = blank
static const uint8_t SEG_MASK[11] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x00};

static lv_obj_t *make_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, radius, 0);
  return o;
}

static void set_digit_segments(Digit &d, int v) {
  uint8_t m = SEG_MASK[v];
  for (int s = 0; s < 7; s++) {
    lv_obj_set_style_bg_color(d.seg[s], lv_color_hex(((m >> s) & 1) ? 0xFFFFFF : 0x161616), 0);
  }
}

static void cover_grow_cb(void *obj, int32_t v) {
  lv_obj_set_height((lv_obj_t *)obj, v);
}
static void cover_shrink_cb(void *obj, int32_t v) {
  lv_obj_set_height((lv_obj_t *)obj, v);
  lv_obj_set_y((lv_obj_t *)obj, DIG_H - v);
}

static void flip_start(int i);

static void flip_done_cb(lv_anim_t *a) {
  int i = (int)(intptr_t)a->user_data;
  lv_obj_add_flag(digits[i].cover, LV_OBJ_FLAG_HIDDEN);
  digits[i].animating = false;
  if (digits[i].cur != digits[i].pending) flip_start(i);
}

static void flip_mid_cb(lv_anim_t *a) {
  int i = (int)(intptr_t)a->user_data;
  Digit &d = digits[i];
  set_digit_segments(d, d.pending);   // peeche cover hai, isliye change dikhta nahi
  d.cur = d.pending;

  lv_anim_t b;
  lv_anim_init(&b);
  lv_anim_set_var(&b, d.cover);
  lv_anim_set_exec_cb(&b, cover_shrink_cb);
  lv_anim_set_values(&b, DIG_H, 0);
  lv_anim_set_time(&b, 140);
  lv_anim_set_ready_cb(&b, flip_done_cb);
  lv_anim_set_user_data(&b, (void *)(intptr_t)i);
  lv_anim_start(&b);
}

static void flip_start(int i) {
  Digit &d = digits[i];
  d.animating = true;
  lv_obj_set_y(d.cover, 0);
  lv_obj_set_height(d.cover, 0);
  lv_obj_clear_flag(d.cover, LV_OBJ_FLAG_HIDDEN);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, d.cover);
  lv_anim_set_exec_cb(&a, cover_grow_cb);
  lv_anim_set_values(&a, 0, DIG_H);
  lv_anim_set_time(&a, 140);
  lv_anim_set_ready_cb(&a, flip_mid_cb);
  lv_anim_set_user_data(&a, (void *)(intptr_t)i);
  lv_anim_start(&a);
}

static void set_digit(int i, int v) {
  Digit &d = digits[i];
  if (d.cur == -1) {                 // pehli baar: seedha dikhao
    set_digit_segments(d, v);
    d.cur = v;
    d.pending = v;
    return;
  }
  d.pending = v;
  if (!d.animating && d.cur != d.pending) flip_start(i);
}

static void make_digit(lv_obj_t *parent, int i, int x, int y) {
  Digit &d = digits[i];
  d.cur = -1;
  d.pending = -1;
  d.animating = false;

  d.cont = lv_obj_create(parent);
  lv_obj_remove_style_all(d.cont);
  lv_obj_clear_flag(d.cont, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  lv_obj_set_pos(d.cont, x, y);
  lv_obj_set_size(d.cont, DIG_W, DIG_H);

  const int dw = DIG_W, dh = DIG_H, dt = DIG_T;
  const int sx[7] = {4, dw - dt, dw - dt, 4, 0, 0, 4};
  const int sy[7] = {0, 4, dh / 2 + 2, dh - dt, dh / 2 + 2, 4, dh / 2 - dt / 2};
  const int sw[7] = {dw - 8, dt, dt, dw - 8, dt, dt, dw - 8};
  const int sh[7] = {dt, dh / 2 - 6, dh / 2 - 6, dt, dh / 2 - 6, dh / 2 - 6, dt};
  for (int s = 0; s < 7; s++) {
    d.seg[s] = make_rect(d.cont, sx[s], sy[s], sw[s], sh[s], 0x161616, 2);
  }

  // flip ke time upar aane wala black cover (sabse upar rehna chahiye)
  d.cover = make_rect(d.cont, 0, 0, DIG_W, 0, 0x000000, 0);
  lv_obj_add_flag(d.cover, LV_OBJ_FLAG_HIDDEN);
}

static void clock_update(const struct tm *ti, bool ok) {
  if (!ok) return;

  int h = ti->tm_hour, m = ti->tm_min;
  int h12 = h % 12;
  if (h12 == 0) h12 = 12;
  set_digit(0, h12 >= 10 ? 1 : 10);   // 10 = blank (leading zero nahi)
  set_digit(1, h12 % 10);
  set_digit(2, m / 10);
  set_digit(3, m % 10);

  static int last_pm = -1;
  if (last_pm != (h >= 12)) {
    last_pm = (h >= 12);
    lv_label_set_text(ampm_label, h >= 12 ? "PM" : "AM");
  }

  static int last_wd = -1;
  if (last_wd != ti->tm_wday) {
    last_wd = ti->tm_wday;
    for (int i = 0; i < 7; i++) {
      lv_obj_set_style_text_color(wd_label[i],
                                  lv_color_hex(ti->tm_wday == (i + 1) % 7 ? C_ORANGE : 0xFFFFFF), 0);
    }
  }

  static int last_day = -1;
  if (last_day != ti->tm_mday) {
    last_day = ti->tm_mday;
    lv_label_set_text_fmt(date_label, "%d/%d", ti->tm_mon + 1, ti->tm_mday);
  }
}

static void build_clock_tab(lv_obj_t *tab) {
  // Weekday list (left)
  static const char *wd[7] = {"MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};
  for (int i = 0; i < 7; i++) {
    wd_label[i] = lv_label_create(tab);
    lv_label_set_text(wd_label[i], wd[i]);
    lv_obj_set_style_text_color(wd_label[i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(wd_label[i], 6, 8 + i * 25);
  }

  // AM / PM
  ampm_label = lv_label_create(tab);
  lv_label_set_text(ampm_label, "AM");
  lv_obj_set_style_text_font(ampm_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(ampm_label, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(ampm_label, 50, 4);

  // Big digits  HH : MM
  make_digit(tab, 0, 48, DIG_Y);
  make_digit(tab, 1, 88, DIG_Y);
  make_digit(tab, 2, 140, DIG_Y);
  make_digit(tab, 3, 180, DIG_Y);
  make_rect(tab, 128, DIG_Y + 28, 8, 8, 0xFFFFFF, 2);
  make_rect(tab, 128, DIG_Y + 64, 8, 8, 0xFFFFFF, 2);
  set_digit(0, 10);
  set_digit(1, 0);
  set_digit(2, 0);
  set_digit(3, 0);

  // Right column (orange)
  temp_label = lv_label_create(tab);
  lv_label_set_text(temp_label, "--°C");
  lv_obj_set_style_text_font(temp_label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(temp_label, lv_color_hex(C_ORANGE), 0);
  lv_obj_set_pos(temp_label, 226, 10);

  wifi_label = lv_label_create(tab);
  lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x555555), 0);
  lv_obj_set_pos(wifi_label, 226, 48);

  lv_obj_t *sync = lv_label_create(tab);
  lv_label_set_text(sync, "AUTO SYNC");
  lv_obj_set_style_text_color(sync, lv_color_hex(C_ORANGE), 0);
  lv_obj_set_pos(sync, 248, 48);

  date_label = lv_label_create(tab);
  lv_label_set_text(date_label, "--/--");
  lv_obj_set_style_text_font(date_label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(date_label, lv_color_hex(C_ORANGE), 0);
  lv_obj_set_pos(date_label, 226, 84);

  lv_obj_t *mo = lv_label_create(tab);
  lv_label_set_text(mo, "MONTH");
  lv_obj_set_style_text_color(mo, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(mo, 226, 122);

  lv_obj_t *dt = lv_label_create(tab);
  lv_label_set_text(dt, "DATE");
  lv_obj_set_style_text_color(dt, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_pos(dt, 280, 122);

  // Weather info (bottom)
  cond_label = lv_label_create(tab);
  lv_label_set_text(cond_label, "Loading...");
  lv_obj_set_style_text_font(cond_label, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(cond_label, lv_color_hex(C_ORANGE), 0);
  lv_obj_set_pos(cond_label, 48, 148);

  hum_label = lv_label_create(tab);
  lv_label_set_text(hum_label, "Humidity --%");
  lv_obj_set_style_text_color(hum_label, lv_color_hex(0x999999), 0);
  lv_obj_set_pos(hum_label, 48, 170);

  lv_obj_t *loc = lv_label_create(tab);
  lv_label_set_text(loc, LOCATION_NAME);
  lv_obj_set_style_text_color(loc, lv_color_hex(0x999999), 0);
  lv_obj_set_pos(loc, 190, 170);

  // Orange glow strip (neeche)
  lv_obj_t *glow = make_rect(tab, 0, 186, 320, 16, 0x000000, 0);
  lv_obj_set_style_bg_grad_color(glow, lv_color_hex(C_ORANGE), 0);
  lv_obj_set_style_bg_grad_dir(glow, LV_GRAD_DIR_VER, 0);
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, uint32_t color, lv_opa_t opa, int w, int h) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(b, opa, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(l);
  return b;
}

// semi-transparent "glass" panel
static lv_obj_t *make_glass(lv_obj_t *parent, int x, int y, int w, int h, int radius, uint32_t color, lv_opa_t opa) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(c, opa, 0);
  lv_obj_set_style_radius(c, radius, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_shadow_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_clear_flag(c, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  return c;
}

static void build_pomodoro_tab(lv_obj_t *tab) {
#if USE_POMO_ANIM
  lv_obj_t *bg = lv_animimg_create(tab);      // animated pixel-art background (320x180)
  lv_animimg_set_src(bg, (const void **)pomo_anim_frames, pomo_anim_count);
  lv_animimg_set_duration(bg, pomo_anim_ms);
  lv_animimg_set_repeat_count(bg, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(bg);
  lv_obj_set_pos(bg, 0, 0);
#elif USE_POMO_BG
  lv_obj_t *bg = lv_img_create(tab);          // static background (320x180)
  lv_img_set_src(bg, &pomo_bg);
  lv_obj_set_pos(bg, 0, 0);
#endif

  // left: frosted round card + timer ring
  make_glass(tab, 18, 12, 128, 128, LV_RADIUS_CIRCLE, 0xFFFFFF, LV_OPA_60);

  pomo_arc = lv_arc_create(tab);
  lv_obj_set_size(pomo_arc, 112, 112);
  lv_obj_set_pos(pomo_arc, 26, 20);
  lv_arc_set_range(pomo_arc, 0, POMO_TOTAL);
  lv_arc_set_rotation(pomo_arc, 270);
  lv_arc_set_bg_angles(pomo_arc, 0, 360);
  lv_arc_set_value(pomo_arc, POMO_TOTAL);
  lv_obj_remove_style(pomo_arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(pomo_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(pomo_arc, 10, LV_PART_MAIN);
  lv_obj_set_style_arc_width(pomo_arc, 10, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(0x9FD6EA), LV_PART_MAIN);
  lv_obj_set_style_arc_color(pomo_arc, lv_color_hex(OC_BLUE), LV_PART_INDICATOR);

  pomo_time = lv_label_create(tab);
  lv_label_set_text(pomo_time, "25:00");
  lv_obj_set_style_text_font(pomo_time, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(pomo_time, lv_color_hex(OC_NAVY), 0);
  lv_obj_align_to(pomo_time, pomo_arc, LV_ALIGN_CENTER, 0, 0);

  // right: dark glass card with title + buttons
  lv_obj_t *card = make_glass(tab, 162, 12, 146, 114, 18, OC_DEEP, LV_OPA_70);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Focus Time");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t *sub = lv_label_create(card);
  lv_label_set_text(sub, "25 min session");
  lv_obj_set_style_text_color(sub, lv_color_hex(0xBFE9F5), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 32);

  lv_obj_t *start_btn = make_btn(card, "Start", OC_VIOLET, LV_OPA_COVER, 120, 32);
  lv_obj_align(start_btn, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_add_event_cb(start_btn, pomo_start_cb, LV_EVENT_CLICKED, NULL);
  pomo_start_label = lv_obj_get_child(start_btn, 0);

  lv_obj_t *reset_btn = make_btn(card, "Reset", 0xFFFFFF, LV_OPA_20, 120, 28);
  lv_obj_align(reset_btn, LV_ALIGN_TOP_MID, 0, 86);
  lv_obj_add_event_cb(reset_btn, pomo_reset_cb, LV_EVENT_CLICKED, NULL);

  lv_timer_create(pomo_timer_cb, 1000, NULL);
}

static void build_switch_tab(lv_obj_t *tab) {
  for (int i = 0; i < 3; i++) make_tile(tab, &sw_tiles[i], i * 104);
}

// =====================================================================
//  DASHBOARD (naya screen)
// =====================================================================
static lv_obj_t *g_tv = NULL;

static void apply_tab_theme(lv_obj_t *tv) {
  lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
  bool clk = (lv_tabview_get_tab_act(tv) == 0);
  lv_obj_set_style_bg_color(btns, lv_color_hex(clk ? 0x000000 : OC_DEEP), 0);
  lv_obj_set_style_text_color(btns, lv_color_hex(clk ? 0x777777 : 0x7FB6CF), 0);
  lv_obj_set_style_text_color(btns, lv_color_hex(clk ? C_ORANGE : 0xFFFFFF), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(btns, lv_color_hex(clk ? 0x1A1A1A : 0x14507A), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btns, lv_color_hex(clk ? C_ORANGE : OC_AQUA), LV_PART_ITEMS | LV_STATE_CHECKED);
}

static void theme_poll_cb(lv_timer_t *t) {
  static int last = -1;
  int a = lv_tabview_get_tab_act(g_tv);
  if (a != last) {
    last = a;
    apply_tab_theme(g_tv);
  }
}

static lv_obj_t *build_dashboard() {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_TOP, 38);   // left-right swipe built-in
  lv_obj_set_size(tv, SCREEN_WIDTH, SCREEN_HEIGHT);
  lv_obj_set_style_bg_color(tv, lv_color_hex(SW_BG), 0);

  lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
  lv_obj_set_style_bg_color(btns, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_text_color(btns, lv_color_hex(C_MUTED), 0);
  lv_obj_set_style_text_font(btns, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(btns, lv_color_hex(C_PINK), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(btns, lv_color_hex(C_PINK_LIGHT), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_border_color(btns, lv_color_hex(C_PINK), LV_PART_ITEMS | LV_STATE_CHECKED);

  lv_obj_t *t1 = lv_tabview_add_tab(tv, "Clock");
  lv_obj_t *t2 = lv_tabview_add_tab(tv, "Pomodoro");
  lv_obj_t *t3 = lv_tabview_add_tab(tv, "Switches");

  lv_obj_t *tabs[] = {t1, t2, t3};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(tabs[i], lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(tabs[i], LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tabs[i], 8, 0);
    lv_obj_clear_flag(tabs[i], LV_OBJ_FLAG_SCROLLABLE);
  }

  lv_obj_set_style_bg_color(t1, lv_color_hex(0x000000), 0);   // clock tab black
  lv_obj_set_style_pad_all(t1, 0, 0);
  lv_obj_set_style_bg_color(t2, lv_color_hex(POMO_GROUND), 0);   // pomodoro: image + ground colour
  lv_obj_set_style_pad_all(t2, 0, 0);
  lv_obj_set_style_bg_color(t3, lv_color_hex(SW_BG), 0);         // switches: dark
  g_tv = tv;
  apply_tab_theme(tv);
  lv_timer_create(theme_poll_cb, 150, NULL);
  build_clock_tab(t1);
  build_pomodoro_tab(t2);
  build_switch_tab(t3);

  lv_timer_create(ui_tick_cb, 1000, NULL);
  return scr;
}

// =====================================================================
//  BOOT SCREEN
// =====================================================================
static void opa_exec_cb(void *obj, int32_t v) {
  lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void show_boot_screen() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Smart Desk\nCompanion");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(C_PINK), 0);
  lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_opa(title, 0, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, title);
  lv_anim_set_exec_cb(&a, opa_exec_cb);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_time(&a, 1200);
  lv_anim_start(&a);

  lv_obj_t *sp = lv_spinner_create(scr, 1000, 60);
  lv_obj_set_size(sp, 34, 34);
  lv_obj_set_style_arc_color(sp, lv_color_hex(C_PINK), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sp, lv_color_hex(0x3A1030), LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
  lv_obj_align(sp, LV_ALIGN_CENTER, 0, 40);

  lv_obj_t *st = lv_label_create(scr);
  lv_label_set_text(st, "Connecting to WiFi...");
  lv_obj_set_style_text_color(st, lv_color_hex(C_PINK_LIGHT), 0);
  lv_obj_align(st, LV_ALIGN_CENTER, 0, 80);
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUZZER, OUTPUT);       digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_DESK_LIGHT, OUTPUT);   digitalWrite(PIN_DESK_LIGHT, RELAY_OFF);
  pinMode(PIN_FAN, OUTPUT);          digitalWrite(PIN_FAN, RELAY_OFF);
  pinMode(PIN_NIGHT_LAMP, OUTPUT);   digitalWrite(PIN_NIGHT_LAMP, RELAY_OFF);

  pinMode(TFT_CS, OUTPUT);   digitalWrite(TFT_CS, HIGH);
  pinMode(TOUCH_CS, OUTPUT); digitalWrite(TOUCH_CS, HIGH);

  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  tft.init(240, 320);
  tft.setRotation(TFT_ROTATION);
  tft.invertDisplay(false);
  tft.fillScreen(ST77XX_BLACK);

  ts.begin(SPI);
  ts.setRotation(TFT_ROTATION);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * 20);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register(&indev_drv);

  // ---- Boot animation + WiFi connect ----
  show_boot_screen();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while ((WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) || millis() - t0 < 2800) {
    lv_timer_handler();
    delay(10);
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? "WiFi connected" : "WiFi FAILED (offline mode)");

  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  xTaskCreatePinnedToCore(weather_task, "weather", 12288, NULL, 1, NULL, 0);

  lv_obj_t *dash = build_dashboard();
  lv_scr_load_anim(dash, LV_SCR_LOAD_ANIM_FADE_ON, 600, 0, true);

  Serial.println("Smart Desk Companion READY");
}

void loop() {
  lv_timer_handler();
  delay(5);
}
