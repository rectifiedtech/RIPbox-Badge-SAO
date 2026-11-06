/*
 * RIPBOX - Hackaday Supercon 2026 SAO badge firmware
 * ESP8266 + SSD1306 64x48 OLED (U8g2) + ESP-NOW
 *
 * ---------------------------------------------------------------------------
 * PINS
 *   GPIO12  Button 1 (LEFT / select)  - INPUT_PULLUP, active LOW
 *   GPIO13  Button 2 (RIGHT / enter)  - INPUT_PULLUP, active LOW
 *   GPIO14  Logo LED                  - HIGH = on
 *   I2C     Wire default pins (GPIO4 = SDA, GPIO5 = SCL), OLED @ 0x3C
 *
 * ---------------------------------------------------------------------------
 * FLASH (EEPROM emulation) MAP  -  EEPROM.begin(1024)
 *   NOTE: offsets below are *virtual* EEPROM offsets. The ESP8266 core maps
 *   them onto one 4 KB flash sector reserved by the linker script.
 *
 *   0        magic byte (0xA5 = flash has been initialised)
 *   1..2     KIOSK ID, uint16 little-endian  (addr 1 = low byte, 2 = high)
 *            Default = 999.   <<< KIOSK ID LIVES AT EEPROM ADDRESS 1 >>>
 *   16       saved message count (0..MSG_MAX)
 *   17       index of oldest saved message (ring buffer head)
 *   18..833  message ring buffer: MSG_MAX records of sizeof(StoredMsg)=51 bytes
 *            record = uint16 fromId + char text[49]
 *
 *   Change the ID without reflashing: open the serial monitor at 115200 and
 *   type  "id 123"  (1..999).  Type "clear" to erase saved messages.
 *
 * ---------------------------------------------------------------------------
 * CONTROLS (summary)
 *   Any screen      : both buttons = secret menu (press again in the secret
 *                     menu to return to the rental screens)
 *   Attract screen  : either button = rental menu
 *   Lists           : LEFT = scroll down, RIGHT = select,
 *                     hold LEFT = back/exit
 *   Rental list     : additionally hold RIGHT = scroll up
 *   Editor          : LEFT = next char, RIGHT = type char,
 *                     hold LEFT = space, hold RIGHT = send
 *                     special chars:  <  backspace   ^  caps   #  exit editor
 *   Flappy          : RIGHT = flap, LEFT = exit
 *   Breaker         : LEFT/RIGHT held = move paddle, both = secret menu
 *   Alerts          : flashing LED + message until any button is pressed
 * ---------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <user_interface.h>
#include <EEPROM.h>
#include <U8g2lib.h>

// ============================== HARDWARE ====================================
#define PIN_BTN_L 12
#define PIN_BTN_R 13
#define PIN_LED   14

U8G2_SSD1306_64X48_ER_F_HW_I2C u8g2(
  U8G2_R0,
  U8X8_PIN_NONE
);

// ============================== CONFIG ======================================
#define FRAME_MS        33        // ~30 fps redraw tick
#define DEBOUNCE_MS     25
#define LONG_MS         650
#define IDLE_RENTAL_MS  30000

#define ESPNOW_CHANNEL  1
#define PKT_MAGIC       0xB7

#define EEPROM_SIZE        1024
#define EE_MAGIC           0xA5
#define EE_ADDR_MAGIC      0
#define EE_ADDR_KIOSK_ID   1      // 2 bytes (uint16 LE)  <-- kiosk ID
#define EE_ADDR_MSG_COUNT  16
#define EE_ADDR_MSG_HEAD   17
#define EE_ADDR_MSG_DATA   18
#define DEFAULT_KIOSK_ID   999

#define MSG_LEN   48
#define MSG_MAX   16
#define ALERT_Q   6

#define FONT_S     u8g2_font_4x6_tf        // 4px wide  -> 16 chars/line
#define FONT_M     u8g2_font_5x7_tf        // 5px wide  -> 12 chars/line
#define FONT_L     u8g2_font_6x10_tf
#define FONT_PRICE u8g2_font_helvB12_tf
#define FONT_NUM   u8g2_font_logisoso16_tn

// ============================== MOVIES ======================================
// ("Mean Girls (2024) (2024)" in the spec was treated as a typo.)
static const char* const MOVIES[] = {
  "Anyone But You (2023)", "Aquaman and the Lost Kingdom (2023)", "Argylle (2024)",
  "Bandit (2022)", "Becky (2020)", "Black Site (2022)", "Bob Marley: One Love (2024)",
  "Breslau (2025)", "By Night's End (2020)", "Chief of Station (2024)", "Civil War (2024)",
  "Domain (2016)", "Dune: Part Two (2024)", "Fast X (2023)", "Ghostbusters: Frozen Empire (2024)",
  "Godzilla x Kong: The New Empire (2024)", "John Wick: Chapter 4 (2023)",
  "Jurassic World Dominion (2022)", "Knox Goes Away (2023)", "Kung Fu Panda 4 (2024)",
  "Land of Bad (2024)", "Madame Web (2024)", "Mean Girls (2024)", "Migration (2023)",
  "No Time to Die (2021)", "One Ranger (2023)", "Rogue Hostage (2021)", "SAS: Red Notice (2021)",
  "Shade (2003)", "Shadow in the Cloud (2020)", "Spider-Man: Across the Spider-Verse (2023)",
  "The Beekeeper (2024)", "The Bricklayer (2023)", "The Fanatic (2019)",
  "The Hunger Games: The Ballad of Songbirds & Snakes (2023)", "The Informer (2019)",
  "The Iron Claw (2023)", "The Super Mario Bros. Movie (2023)", "Top Gun: Maverick (2022)",
  "Vendetta (2022)", "Wonka (2023)"
};
#define MOVIE_COUNT (sizeof(MOVIES) / sizeof(MOVIES[0]))

// ============================== TYPES =======================================
enum State : uint8_t {
  ST_BOOT, ST_ATTRACT, ST_RENTAL_LIST, ST_RENTAL_DETAIL, ST_PAYMENT,
  ST_SECRET, ST_MESSAGES, ST_MSG_VIEW, ST_SCAN, ST_KIOSK_MENU, ST_EDITOR,
  ST_TX, ST_GAMES, ST_FLAPPY, ST_BREAKER, ST_ALERT
};
enum Ev : uint8_t { EV_NONE, EV_L, EV_R, EV_L_LONG, EV_R_LONG, EV_BOTH };
enum { PKT_SCAN = 1, PKT_HELLO, PKT_PING, PKT_MSG, PKT_ACK };
enum { ALERT_PING = 1, ALERT_MSG = 2 };

struct __attribute__((packed)) Packet {
  uint8_t  magic;
  uint8_t  type;
  uint8_t  seq;
  uint16_t src;
  uint16_t dst;                 // 0 = anyone
  char     text[MSG_LEN + 1];
};
#define PKT_HDR 7               // bytes before text[]

struct __attribute__((packed)) StoredMsg {
  uint16_t from;
  char     text[MSG_LEN + 1];
};

struct Alert { uint8_t kind; uint16_t from; char text[MSG_LEN + 1]; };

// Must live up here: the Arduino preprocessor auto-generates function
// prototypes above the first function, so types used in signatures
// have to be declared before that point.
typedef void (*LabelFn)(int idx, char* out, size_t n);

// ============================== GLOBALS =====================================
static State    state = ST_BOOT, resumeState = ST_ATTRACT;
static uint32_t stateStart = 0, lastFrame = 0, lastInput = 0, selChangedAt = 0;
static bool     dirty = true;

static uint16_t myId = DEFAULT_KIOSK_ID;
static uint8_t  msgCount = 0, msgHead = 0;

// menu selections
static int rentalSel = 0, secretSel = 0, msgSel = 0, scanSel = 0, kioskSel = 0, gamesSel = 0;
static int viewIdx = 0;
static bool msgFromAlert = false;
static char viewText[MSG_LEN + 1];
static uint16_t viewFrom = 0;
static uint16_t kioskTarget = 0;

// buttons
static const uint8_t BTN_PINS[2] = { PIN_BTN_L, PIN_BTN_R };
static bool bRaw[2], bStable[2], bLong[2], bSuppress = false, gPressLatch[2];
static uint32_t bChg[2], bDownAt[2];

// radio
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
#define RXQ 8
static Packet rxQ[RXQ];
static volatile uint8_t rxHead = 0, rxTail = 0;
static bool radioOk = false;
static uint8_t txSeq = 0;

struct TxItem { bool used; uint8_t type; uint16_t dst; uint8_t seq; uint32_t due; };
static TxItem txQ[6];

static struct {
  bool active, done, ok; uint8_t kind, tries; uint16_t target; uint8_t seq;
  uint32_t last; char text[MSG_LEN + 1];
} tx;

struct Seen { uint16_t id; uint32_t last; };
static Seen seen[8];
static uint8_t seenCount = 0;
static uint32_t scanNextAt = 0;

struct Dup { uint16_t src; uint8_t seq; uint32_t t; };
static Dup dups[8];
static uint8_t dupIdx = 0;

static Alert alerts[ALERT_Q];
static uint8_t alertCount = 0;
static uint32_t alertSince = 0;

// editor
static const char CHARSET[] = "abcdefghijklmnopqrstuvwxyz0123456789.,!?'-:@/<^#";
#define CHARSET_N (sizeof(CHARSET) - 1)
static char edText[MSG_LEN + 1];
static uint8_t edLen = 0, edSel = 0;
static bool edCaps = false;
static uint16_t edTarget = 0;
static const char* edNotice = nullptr;
static uint32_t edNoticeUntil = 0;

// flappy
#define BIRD_X 12
#define BIRD_W 5
#define BIRD_H 4
#define PIPE_W 8
#define PIPE_GAP 18
#define PIPE_SP 34
struct Pipe { float x; int gapY; bool passed; };
static Pipe pipes[3];
static float fbY, fbVy;
static int fbScore, fbBest = 0;
static uint8_t fbMode;   // 0 ready, 1 play, 2 over
static uint32_t fbModeAt;

// breaker
#define PADDLE_Y 45
#define PADDLE_W 14
static uint8_t brk[4];
static int bricksLeft, bScore, bLives;
static float bx, by, bvx, bvy, bpx, bspd;
static uint8_t bMode;    // 0 ready, 1 play, 2 over
static uint32_t bModeAt;

// ============================== SMALL HELPERS ===============================
static void setState(State s) { state = s; stateStart = millis(); dirty = true; }

static void drawCentered(int baseline, const char* s, int dx = 0) {
  u8g2.drawStr((64 - u8g2.getStrWidth(s)) / 2 + dx, baseline, s);
}
static void drawRightStr(int baseline, const char* s) {
  u8g2.drawStr(64 - u8g2.getStrWidth(s), baseline, s);
}
static float easeOut(float t) { if (t < 0) t = 0; if (t > 1) t = 1; return 1.0f - (1.0f - t) * (1.0f - t); }

// Draw string inside a clip box; scrolls (marquee) if wider than the box.
static void drawMarquee(const char* s, int x0, int top, int width, int h, int baseline, bool scroll) {
  int w = u8g2.getStrWidth(s);
  u8g2.setClipWindow(x0, top, x0 + width, top + h);
  if (w <= width || !scroll) {
    u8g2.drawStr(x0, baseline, s);
  } else {
    uint32_t e = millis() - selChangedAt;
    int off = 0;
    if (e > 700) off = ((e - 700) / 45) % (w + 12);
    u8g2.drawStr(x0 - off, baseline, s);
    u8g2.drawStr(x0 - off + w + 12, baseline, s);
  }
  u8g2.setMaxClipWindow();
}

static void moveSel(int& sel, int count) { sel = (sel + 1) % count; selChangedAt = millis(); dirty = true; }

// ============================== FLASH STORAGE ===============================
static void saveId(uint16_t id) {
  EEPROM.write(EE_ADDR_KIOSK_ID, id & 0xFF);
  EEPROM.write(EE_ADDR_KIOSK_ID + 1, id >> 8);
  EEPROM.commit();
}

static void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(EE_ADDR_MAGIC) != EE_MAGIC) {          // first boot: init flash
    EEPROM.write(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.write(EE_ADDR_KIOSK_ID, DEFAULT_KIOSK_ID & 0xFF);
    EEPROM.write(EE_ADDR_KIOSK_ID + 1, DEFAULT_KIOSK_ID >> 8);
    EEPROM.write(EE_ADDR_MSG_COUNT, 0);
    EEPROM.write(EE_ADDR_MSG_HEAD, 0);
    EEPROM.commit();
  }
  myId = EEPROM.read(EE_ADDR_KIOSK_ID) | (EEPROM.read(EE_ADDR_KIOSK_ID + 1) << 8);
  if (myId < 1 || myId > 999) myId = DEFAULT_KIOSK_ID;
  msgCount = EEPROM.read(EE_ADDR_MSG_COUNT);
  msgHead  = EEPROM.read(EE_ADDR_MSG_HEAD);
  if (msgCount > MSG_MAX || msgHead >= MSG_MAX) { msgCount = 0; msgHead = 0; }
}

static void readStored(uint8_t n, StoredMsg& m) {        // n = 0 is oldest
  uint8_t slot = (msgHead + n) % MSG_MAX;
  EEPROM.get(EE_ADDR_MSG_DATA + slot * (int)sizeof(StoredMsg), m);
  m.text[MSG_LEN] = 0;
}

static void storeMessage(uint16_t from, const char* text) {
  StoredMsg m;
  memset(&m, 0, sizeof(m));
  m.from = from;
  strncpy(m.text, text, MSG_LEN);
  uint8_t slot;
  if (msgCount < MSG_MAX) { slot = (msgHead + msgCount) % MSG_MAX; msgCount++; }
  else { slot = msgHead; msgHead = (msgHead + 1) % MSG_MAX; }     // overwrite oldest
  EEPROM.put(EE_ADDR_MSG_DATA + slot * (int)sizeof(StoredMsg), m);
  EEPROM.write(EE_ADDR_MSG_COUNT, msgCount);
  EEPROM.write(EE_ADDR_MSG_HEAD, msgHead);
  EEPROM.commit();
}

static void serialService() {
  static char buf[24]; static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      buf[n] = 0;
      if (n) {
        if (strncmp(buf, "id ", 3) == 0) {
          int v = atoi(buf + 3);
          if (v >= 1 && v <= 999) { myId = v; saveId(myId); Serial.printf("Kiosk ID set to %03d\n", v); dirty = true; }
          else Serial.println("ID must be 1..999");
        } else if (strcmp(buf, "clear") == 0) {
          msgCount = 0; msgHead = 0;
          EEPROM.write(EE_ADDR_MSG_COUNT, 0); EEPROM.write(EE_ADDR_MSG_HEAD, 0); EEPROM.commit();
          Serial.println("Messages cleared");
        }
      }
      n = 0;
    } else if (n < sizeof(buf) - 1) buf[n++] = c;
  }
}

// ============================== BUTTONS =====================================
static Ev readButtons() {
  uint32_t now = millis();
  Ev ev = EV_NONE;
  int8_t edge[2] = { 0, 0 };
  for (int i = 0; i < 2; i++) {
    bool raw = digitalRead(BTN_PINS[i]) == LOW;
    if (raw != bRaw[i]) { bRaw[i] = raw; bChg[i] = now; }
    if (bStable[i] != bRaw[i] && now - bChg[i] >= DEBOUNCE_MS) { bStable[i] = bRaw[i]; edge[i] = raw ? 1 : -1; }
  }
  for (int i = 0; i < 2; i++) if (edge[i] == 1) {
    gPressLatch[i] = true; bDownAt[i] = now; bLong[i] = false;
    if (!bSuppress && bStable[1 - i]) { bSuppress = true; ev = EV_BOTH; }
  }
  for (int i = 0; i < 2; i++) if (edge[i] == -1) {
    if (bSuppress) { if (!bStable[0] && !bStable[1]) bSuppress = false; }
    else if (!bLong[i] && ev == EV_NONE) ev = i ? EV_R : EV_L;
  }
  if (!bSuppress && ev == EV_NONE) {
    for (int i = 0; i < 2; i++) {
      if (bStable[i] && !bLong[i] && !bStable[1 - i] && now - bDownAt[i] >= LONG_MS) {
        bLong[i] = true; ev = i ? EV_R_LONG : EV_L_LONG; break;
      }
    }
  }
  return ev;
}

// ============================== RADIO =======================================
static void onRecv(uint8_t* mac, uint8_t* data, uint8_t len) {   // runs in SDK context: keep tiny
  if (len < PKT_HDR || data[0] != PKT_MAGIC) return;
  uint8_t next = (rxHead + 1) % RXQ;
  if (next == rxTail) return;
  memset(&rxQ[rxHead], 0, sizeof(Packet));
  memcpy(&rxQ[rxHead], data, len < sizeof(Packet) ? len : sizeof(Packet));
  rxHead = next;
}

static void radioInit() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleepMode(WIFI_NONE_SLEEP);          // radio must stay awake to receive
  wifi_set_channel(ESPNOW_CHANNEL);
  if (esp_now_init() != 0) { radioOk = false; return; }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onRecv);
  esp_now_add_peer((uint8_t*)BCAST, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
  radioOk = true;
}

static void sendPacket(const Packet& p, size_t len) {
  if (radioOk) esp_now_send((uint8_t*)BCAST, (uint8_t*)&p, len);
}
static void sendSimple(uint8_t type, uint16_t dst, uint8_t seq) {
  Packet p; memset(&p, 0, sizeof(p));
  p.magic = PKT_MAGIC; p.type = type; p.seq = seq; p.src = myId; p.dst = dst;
  sendPacket(p, PKT_HDR);
}
static void queueTx(uint8_t type, uint16_t dst, uint8_t seq, uint32_t delayMs) {
  for (auto& t : txQ) if (!t.used) { t = { true, type, dst, seq, millis() + delayMs }; return; }
}
static void txQueueService() {
  uint32_t now = millis();
  for (auto& t : txQ) if (t.used && (int32_t)(now - t.due) >= 0) { sendSimple(t.type, t.dst, t.seq); t.used = false; }
}

static void txStart(uint8_t kind, uint16_t target, const char* text) {   // kind 0 = msg, 1 = ping
  tx.kind = kind; tx.target = target; tx.seq = txSeq++; tx.tries = 0;
  tx.active = true; tx.done = false; tx.ok = false; tx.last = 0;
  strlcpy(tx.text, text ? text : "", sizeof(tx.text));
}
static void txService() {                      // send / retry until ACK or give up
  if (!tx.active) return;
  uint32_t now = millis();
  if (tx.tries == 0 || now - tx.last >= 500) {
    if (tx.tries >= 4) { tx.active = false; tx.done = true; tx.ok = false; dirty = true; return; }
    if (tx.kind == 0) {
      Packet p; memset(&p, 0, sizeof(p));
      p.magic = PKT_MAGIC; p.type = PKT_MSG; p.seq = tx.seq; p.src = myId; p.dst = tx.target;
      strlcpy(p.text, tx.text, sizeof(p.text));
      sendPacket(p, sizeof(p));
    } else sendSimple(PKT_PING, tx.target, tx.seq);
    tx.tries++; tx.last = now; dirty = true;
  }
}

static void noteKiosk(uint16_t id) {
  uint32_t now = millis();
  for (uint8_t i = 0; i < seenCount; i++) if (seen[i].id == id) { seen[i].last = now; return; }
  if (seenCount >= 8) return;
  uint8_t pos = seenCount++;                    // insert sorted by id
  while (pos > 0 && seen[pos - 1].id > id) { seen[pos] = seen[pos - 1]; pos--; }
  seen[pos] = { id, now };
  dirty = true;
}
static void pruneSeen() {
  uint32_t now = millis(); uint8_t j = 0;
  for (uint8_t i = 0; i < seenCount; i++) if (now - seen[i].last < 8000) seen[j++] = seen[i];
  if (j != seenCount) dirty = true;
  seenCount = j;
}

static bool isDup(uint16_t src, uint8_t seq) {
  uint32_t now = millis();
  for (auto& d : dups) if (d.t && d.src == src && d.seq == seq && now - d.t < 15000) return true;
  return false;
}
static void rememberDup(uint16_t src, uint8_t seq) { dups[dupIdx] = { src, seq, millis() }; dupIdx = (dupIdx + 1) % 8; }

static void pushAlert(uint8_t kind, uint16_t from, const char* text) {
  if (alertCount >= ALERT_Q) return;
  Alert& a = alerts[alertCount++];
  a.kind = kind; a.from = from;
  strlcpy(a.text, text, sizeof(a.text));
  for (char* c = a.text; *c; c++) if (*c < 32 || *c > 126) *c = '?';
}
static void popAlert() {
  if (!alertCount) return;
  for (uint8_t i = 1; i < alertCount; i++) alerts[i - 1] = alerts[i];
  alertCount--;
}

static void handlePacket(const Packet& p) {
  if (p.src < 1 || p.src > 999) return;
  noteKiosk(p.src);
  bool forMe = (p.dst == myId);
  switch (p.type) {
    case PKT_SCAN:                              // answer discovery, jittered to avoid collisions
      queueTx(PKT_HELLO, p.src, 0, random(10, 160)); break;
    case PKT_HELLO: break;                      // noteKiosk() above did the work
    case PKT_PING:
    case PKT_MSG:
      if (!forMe) break;
      if (isDup(p.src, p.seq)) { queueTx(PKT_ACK, p.src, p.seq, 0); break; }   // retry: re-ACK only
      if (alertCount < ALERT_Q) {               // no room = no ACK, sender sees "no reply"
        queueTx(PKT_ACK, p.src, p.seq, 0);
        rememberDup(p.src, p.seq);
        pushAlert(p.type == PKT_PING ? ALERT_PING : ALERT_MSG, p.src, p.type == PKT_MSG ? p.text : "");
      }
      break;
    case PKT_ACK:
      if (forMe && tx.active && p.seq == tx.seq && p.src == tx.target) {
        tx.active = false; tx.done = true; tx.ok = true; dirty = true;
        if (tx.kind == 0) { edLen = 0; edText[0] = 0; }          // clear editor on success
      }
      break;
  }
}

static void rxService() {
  while (rxTail != rxHead) {
    Packet p = rxQ[rxTail];
    rxTail = (rxTail + 1) % RXQ;
    p.text[MSG_LEN] = 0;
    handlePacket(p);
  }
}

// ============================== TEXT WRAP ===================================
static uint8_t wrapText(const char* s, char lines[][17], uint8_t maxLines) {
  uint8_t n = 0; size_t len = strlen(s), i = 0;
  while (i < len && n < maxLines) {
    size_t rem = len - i, take = rem <= 16 ? rem : 16;
    if (rem > 16) {
      int br = -1;
      for (int k = 16; k > 0; k--) if (s[i + k] == ' ') { br = k; break; }
      if (br > 0) take = br;
    }
    memcpy(lines[n], s + i, take); lines[n][take] = 0;
    i += take;
    while (i < len && s[i] == ' ') i++;
    n++;
  }
  return n;
}

// ============================== DRAWING =====================================
static void drawList(const char* title, const char* right, int count, int sel, LabelFn fn) {
  u8g2.setFont(FONT_S);
  u8g2.drawStr(0, 6, title);
  if (right) drawRightStr(6, right);
  u8g2.drawHLine(0, 8, 64);
  int first = sel - 2;
  if (first > count - 5) first = count - 5;
  if (first < 0) first = 0;
  char buf[64];
  for (int r = 0; r < 5; r++) {
    int idx = first + r;
    if (idx >= count) break;
    int top = 10 + r * 7;
    fn(idx, buf, sizeof(buf));
    bool s = (idx == sel);
    if (s) { u8g2.drawBox(0, top, 62, 7); u8g2.setDrawColor(0); }
    drawMarquee(buf, 1, top, 60, 7, top + 5, s);
    u8g2.setDrawColor(1);
  }
  if (count > 5) {                                  // scroll thumb
    int th = max(3, 35 * 5 / count);
    int ty = 10 + (35 - th) * sel / (count - 1);
    u8g2.drawBox(62, ty, 2, th);
  }
}

static void idStr2(char* o, size_t n) { snprintf(o, n, "%02u", (unsigned)(myId % 100)); }

static void drawLogo() {
  char buf[8];
  u8g2.drawRFrame(0, 0, 64, 48, 4);
  u8g2.drawRBox(3, 3, 58, 13, 2);
  u8g2.setFont(FONT_L); u8g2.setDrawColor(0);
  drawCentered(13, "RIPBOX");
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_S); drawCentered(24, "KIOSK ID");
  snprintf(buf, sizeof(buf), "%03u", (unsigned)myId);
  u8g2.setFont(FONT_NUM); drawCentered(44, buf);
}

static void drawIcon(uint8_t k, int cx, int cy, uint32_t t) {
  switch (k) {
    case 0: {                                           // spinning disc
      u8g2.drawCircle(cx, cy, 10); u8g2.drawCircle(cx, cy, 2);
      float a = t * 0.006f;
      u8g2.drawLine(cx + cosf(a) * 4, cy + sinf(a) * 4, cx + cosf(a) * 9, cy + sinf(a) * 9);
      u8g2.drawLine(cx - cosf(a) * 4, cy - sinf(a) * 4, cx - cosf(a) * 9, cy - sinf(a) * 9);
      break; }
    case 1: {                                           // scrolling film strip
      u8g2.drawFrame(cx - 20, cy - 9, 40, 18);
      int off = (t / 60) % 6;
      for (int x = cx - 26 + off; x < cx + 20; x += 6)
        if (x >= cx - 19 && x <= cx + 17) { u8g2.drawBox(x, cy - 8, 2, 2); u8g2.drawBox(x, cy + 6, 2, 2); }
      u8g2.drawFrame(cx - 14, cy - 4, 12, 8); u8g2.drawFrame(cx + 2, cy - 4, 12, 8);
      break; }
    case 2: {                                           // credit card
      u8g2.drawRFrame(cx - 14, cy - 9, 28, 18, 2);
      u8g2.drawBox(cx - 14, cy - 5, 28, 4);
      u8g2.drawBox(cx - 10, cy + 2, 5, 4);
      if ((t / 300) & 1) u8g2.drawBox(cx + 6, cy + 5, 4, 1);
      break; }
    default: {                                          // tombstone
      u8g2.drawRFrame(cx - 9, cy - 10, 18, 20, 8);
      u8g2.drawVLine(cx, cy - 6, 9); u8g2.drawHLine(cx - 3, cy - 3, 7);
      u8g2.drawHLine(cx - 13, cy + 10, 26);
      break; }
  }
}

struct Ad { const char* head; const char* l1; const char* l2; uint8_t icon; };
static const Ad ADS[] = {
  { "RENT A MOVIE", "JUST $1.99/NIGHT", "",          0 },
  { "NEW RELEASES", "EVERY TUESDAY",    "",          1 },
  { "SWIPE & GO",   "JUST TAP A MOVIE", "",          2 },
  { "RETURN IT",    "AT ANY KIOSK",     "BY 9PM",    0 },
  { "RIP REDBOX",   "GONE BUT NOT",     "FORGOTTEN", 3 },
  { "PRESS ANY",    "BUTTON TO RENT",   "A MOVIE",   1 },
};
#define AD_COUNT (sizeof(ADS) / sizeof(ADS[0]))
#define AD_MS 2800

static void renderAttract() {
  uint32_t t = millis() - stateStart;
  uint32_t idx = (t / AD_MS) % AD_COUNT, lt = t % AD_MS;
  const Ad& ad = ADS[idx];
  int dx = 0;
  if (lt < 350) dx = (int)((1.0f - easeOut(lt / 350.0f)) * 64);
  else if (lt > AD_MS - 250) dx = -(int)(easeOut((lt - (AD_MS - 250)) / 250.0f) * 64);
  drawIcon(ad.icon, 32 + dx, 13, t);
  u8g2.setFont(FONT_M); drawCentered(32, ad.head, dx);
  u8g2.setFont(FONT_S);
  drawCentered(39, ad.l1, dx);
  if (ad.l2[0]) drawCentered(46, ad.l2, dx);
}

// ---- rental
static void lblMovie(int i, char* o, size_t n) {
  if (i < (int)MOVIE_COUNT) strlcpy(o, MOVIES[i], n); else strlcpy(o, "< EXIT", n);
}
static void priceStr(int idx, char* o, size_t n) {
  unsigned c = 150 + 25 * ((idx * 7 + 3) % 6);          // fictitious $1.50 - $2.75
  snprintf(o, n, "$%u.%02u", c / 100, c % 100);
}
static void renderRentalList() {
  char t[20]; snprintf(t, sizeof(t), "RENT %d/%d", min(rentalSel + 1, (int)MOVIE_COUNT), (int)MOVIE_COUNT);
  drawList(t, nullptr, MOVIE_COUNT + 1, rentalSel, lblMovie);
}
static void renderRentalDetail() {
  char p[10]; priceStr(rentalSel, p, sizeof(p));
  u8g2.setFont(FONT_S);
  u8g2.drawBox(0, 0, 64, 8); u8g2.setDrawColor(0);
  drawMarquee(MOVIES[rentalSel], 1, 0, 62, 8, 6, true);
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_PRICE); drawCentered(25, p);
  u8g2.setFont(FONT_S);
  drawCentered(34, "1 NIGHT, 1 DISC");
  drawCentered(41, "+ TAX & JOY");
  u8g2.drawStr(0, 47, "<BACK"); drawRightStr(47, "PAY>");
}

#define PAY_MS 7000
static void renderPayment() {
  uint32_t t = millis() - stateStart;
  u8g2.setFont(FONT_S);
  if (t < 1500) {                                       // card slides into slot
    drawCentered(7, "INSERT CARD");
    float e = easeOut(t / 1400.0f);
    u8g2.setClipWindow(0, 0, 64, 28);
    u8g2.drawRFrame(19, (int)(8 + e * 18) - 12, 26, 16, 2);
    u8g2.drawBox(19, (int)(8 + e * 18) - 8, 26, 3);
    u8g2.setMaxClipWindow();
    u8g2.drawFrame(14, 28, 36, 3);
    u8g2.drawRFrame(8, 16, 48, 30, 3);
  } else if (t < 3300) {                                // processing
    drawCentered(14, "PROCESSING");
    float f = (t - 1500) / 1800.0f;
    u8g2.drawFrame(8, 22, 48, 8);
    u8g2.drawBox(10, 24, (int)(44 * f), 4);
    int d = (t / 200) % 4; char dots[5] = "....";
    dots[d] = 0; drawCentered(42, dots);
  } else if (t < 4300) {                                // approved
    bool on = ((t / 150) & 1);
    if (on) { u8g2.drawBox(4, 14, 56, 18); u8g2.setDrawColor(0); }
    u8g2.setFont(FONT_M); drawCentered(26, "APPROVED!");
    u8g2.setDrawColor(1);
  } else {                                              // dispense disc
    float e = easeOut((t - 4300) / 1600.0f);
    drawCentered(7, "DISPENSING");
    u8g2.drawFrame(10, 11, 44, 3);
    int cy = 14 + (int)(e * 20);
    u8g2.setClipWindow(0, 13, 64, 48);
    u8g2.drawCircle(32, cy, 7); u8g2.drawCircle(32, cy, 1);
    u8g2.setMaxClipWindow();
    u8g2.drawFrame(6, 38, 52, 4);
    if (t > 5900) drawCentered(47, "ENJOY! RIP");
  }
}

// ---- secret menu
static void lblSecret(int i, char* o, size_t n) {
  switch (i) {
    case 0: snprintf(o, n, "MESSAGES (%u)", msgCount); break;
    case 1: strlcpy(o, "SCAN KIOSKS", n); break;
    case 2: strlcpy(o, "GAMES", n); break;
    default: strlcpy(o, "< EXIT", n);
  }
}
static void lblMsg(int i, char* o, size_t n) {
  if (i >= msgCount) { strlcpy(o, "< BACK", n); return; }
  StoredMsg m; readStored(i, m);
  snprintf(o, n, "%d %03u %s", i + 1, (unsigned)m.from, m.text);
}
static void lblScan(int i, char* o, size_t n) {
  if (i < seenCount) snprintf(o, n, "KIOSK %03u", (unsigned)seen[i].id); else strlcpy(o, "< BACK", n);
}
static void lblKiosk(int i, char* o, size_t n) {
  const char* l[] = { "SEND MESSAGE", "PING KIOSK", "< BACK" }; strlcpy(o, l[i], n);
}
static void lblGames(int i, char* o, size_t n) {
  const char* l[] = { "FLAPPY RIP", "BLOCK BREAKER", "< BACK" }; strlcpy(o, l[i], n);
}

static void renderMsgView() {
  char hdr[16], num[12]; char lines[5][17];
  snprintf(hdr, sizeof(hdr), "FROM %03u", (unsigned)viewFrom);
  u8g2.setFont(FONT_S); u8g2.drawStr(0, 6, hdr);
  if (!msgFromAlert) { snprintf(num, sizeof(num), "%d/%d", viewIdx + 1, msgCount); drawRightStr(6, num); }
  u8g2.drawHLine(0, 8, 64);
  uint8_t n = wrapText(viewText, lines, 5);
  for (uint8_t i = 0; i < n; i++) u8g2.drawStr(0, 16 + i * 7, lines[i]);
}

static void renderScan() {
  char t[12], id[4]; idStr2(id, sizeof(id));
  const char* dots[] = { "SCAN", "SCAN.", "SCAN..", "SCAN..." };
  strlcpy(t, dots[(millis() / 300) % 4], sizeof(t));
  drawList(t, id, seenCount + 1, scanSel, lblScan);
  if (seenCount == 0) { u8g2.setFont(FONT_S); drawCentered(26, "LOOKING FOR"); drawCentered(33, "KIOSKS..."); }
}

// ---- editor
static char tokenChar(uint8_t idx) {
  char c = CHARSET[idx % CHARSET_N];
  if (edCaps && c >= 'a' && c <= 'z') c -= 32;
  return c;
}
static void renderEditor() {
  char hdr[12], cnt[8], s[2] = { 0, 0 };
  snprintf(hdr, sizeof(hdr), "TO %03u", (unsigned)edTarget);
  snprintf(cnt, sizeof(cnt), "%u/%u", edLen, MSG_LEN);
  u8g2.setFont(FONT_S);
  u8g2.drawStr(0, 6, hdr); drawRightStr(6, cnt);
  u8g2.drawHLine(0, 8, 64);
  for (int l = 0; l < 3; l++) {
    char line[17]; int start = l * 16;
    if (start > edLen) break;
    int n = edLen - start; if (n > 16) n = 16;
    memcpy(line, edText + start, n); line[n] = 0;
    u8g2.drawStr(0, 15 + l * 7, line);
  }
  if (edLen < MSG_LEN && ((millis() / 400) & 1)) {            // blinking cursor
    u8g2.drawHLine((edLen % 16) * 4, 15 + (edLen / 16) * 7 + 1, 3);
  }
  // selector strip
  u8g2.drawBox(27, 31, 10, 11);
  for (int k = -3; k <= 3; k++) {
    s[0] = tokenChar((edSel + k + CHARSET_N * 2) % CHARSET_N);
    int cx = 32 + k * (k == 0 ? 0 : 8) + (k > 0 ? 3 : (k < 0 ? -3 : 0));
    if (k == 0) { u8g2.setFont(FONT_L); u8g2.setDrawColor(0); u8g2.drawStr(cx - 3, 39, s); u8g2.setDrawColor(1); u8g2.setFont(FONT_S); }
    else u8g2.drawStr(cx - 2, 38, s);
  }
  // hint line
  const char* hint;
  char cur = CHARSET[edSel];
  if (edNotice && millis() < edNoticeUntil) hint = edNotice;
  else if (cur == '<') hint = "R: DELETE";
  else if (cur == '^') hint = edCaps ? "R: CAPS OFF" : "R: CAPS ON";
  else if (cur == '#') hint = "R: EXIT";
  else {
    static const char* h[] = { "L:NEXT R:TYPE", "HOLD L: SPACE", "HOLD R: SEND" };
    hint = h[(millis() / 1500) % 3];
  }
  drawCentered(47, hint);
}

static void renderTx() {
  char id[8]; snprintf(id, sizeof(id), "%03u", (unsigned)tx.target);
  u8g2.setFont(FONT_S);
  u8g2.drawStr(0, 6, tx.kind == 0 ? "SENDING" : "PINGING"); drawRightStr(6, id);
  u8g2.drawHLine(0, 8, 64);
  if (!tx.done) {
    drawCentered(24, "WAITING FOR");
    drawCentered(31, "REPLY");
    char tr[12]; snprintf(tr, sizeof(tr), "TRY %u/4", tx.tries); drawCentered(40, tr);
    int d = (millis() / 250) % 4;
    for (int i = 0; i < 4; i++) if (i <= d) u8g2.drawBox(20 + i * 8, 44, 4, 3);
  } else if (tx.ok) {
    u8g2.drawLine(22, 22, 28, 28); u8g2.drawLine(23, 22, 29, 28);
    u8g2.drawLine(28, 28, 42, 14); u8g2.drawLine(29, 28, 43, 14);
    drawCentered(36, tx.kind == 0 ? "DELIVERED!" : "PING ACKED!");
    drawCentered(47, "PRESS A BUTTON");
  } else {
    u8g2.drawLine(24, 12, 38, 26); u8g2.drawLine(38, 12, 24, 26);
    drawCentered(36, "NO REPLY");
    drawCentered(42, "OUT OF RANGE?");
    drawCentered(48, "PRESS A BUTTON");
  }
}

static void renderAlert() {
  if (!alertCount) return;
  const Alert& a = alerts[0];
  bool on = ((millis() / 130) & 1);
  char id[16];
  u8g2.drawFrame(0, 0, 64, 48);
  if (on) u8g2.drawFrame(1, 1, 62, 46);
  u8g2.setFont(FONT_S);
  snprintf(id, sizeof(id), "KIOSK ID %03u", (unsigned)a.from);
  if (a.kind == ALERT_PING) {
    drawCentered(13, id); drawCentered(21, "JUST");
    drawCentered(29, "PINGED YOU!");
    drawCentered(42, "PRESS A BUTTON");
  } else {
    drawCentered(13, "NEW MSG FROM"); drawCentered(21, id);
    drawCentered(33, "PUSH TO VIEW");
  }
  if (alertCount > 1) { char q[8]; snprintf(q, sizeof(q), "+%u", alertCount - 1); u8g2.drawStr(4, 44, q); }
}

// ---- games
static void flappyReset() {
  fbY = 20; fbVy = 0; fbScore = 0; fbMode = 0; fbModeAt = millis();
  for (int i = 0; i < 3; i++) { pipes[i].x = 72 + i * PIPE_SP; pipes[i].gapY = random(6, 27); pipes[i].passed = false; }
  gPressLatch[0] = gPressLatch[1] = false;
}
static void flappyUpdate(float dt) {
  if (fbMode == 0) {
    fbY = 20 + sinf(millis() * 0.008f) * 2;
    if (gPressLatch[1]) { fbMode = 1; fbVy = -75; }
  } else if (fbMode == 1) {
    if (gPressLatch[1]) fbVy = -75;
    fbVy += 230 * dt; fbY += fbVy * dt;
    if (fbY < 0) { fbY = 0; fbVy = 0; }
    bool hit = (fbY + BIRD_H >= 47);
    for (auto& p : pipes) {
      p.x -= 26 * dt;
      if (p.x < -PIPE_W - 2) { p.x += 3 * PIPE_SP; p.gapY = random(6, 27); p.passed = false; }
      if (!p.passed && p.x + PIPE_W < BIRD_X) { p.passed = true; fbScore++; }
      if (p.x - 1 < BIRD_X + BIRD_W && p.x + PIPE_W + 1 > BIRD_X)
        if (fbY < p.gapY || fbY + BIRD_H > p.gapY + PIPE_GAP) hit = true;
    }
    if (hit) { fbMode = 2; fbModeAt = millis(); if (fbScore > fbBest) fbBest = fbScore; }
  } else if (millis() - fbModeAt > 500 && gPressLatch[1]) flappyReset();
  gPressLatch[0] = gPressLatch[1] = false;
}
static void renderFlappy() {
  for (auto& p : pipes) {
    int px = (int)p.x;
    u8g2.drawBox(px, 0, PIPE_W, p.gapY); u8g2.drawBox(px - 1, p.gapY - 2, PIPE_W + 2, 2);
    u8g2.drawBox(px, p.gapY + PIPE_GAP, PIPE_W, 48 - p.gapY - PIPE_GAP);
    u8g2.drawBox(px - 1, p.gapY + PIPE_GAP, PIPE_W + 2, 2);
  }
  int y = (int)fbY;
  u8g2.drawBox(BIRD_X - 1, y - 1, BIRD_W + 3, BIRD_H + 2);          // outline "halo" for contrast
  u8g2.setDrawColor(0); u8g2.drawBox(BIRD_X, y, BIRD_W, BIRD_H);
  u8g2.setDrawColor(1); u8g2.drawPixel(BIRD_X + 3, y + 1); u8g2.drawHLine(BIRD_X, y + 3, 2);
  u8g2.drawHLine(0, 47, 64);
  char s[8]; snprintf(s, sizeof(s), "%d", fbScore);
  u8g2.setFont(FONT_S);
  int w = u8g2.getStrWidth(s);
  u8g2.setDrawColor(0); u8g2.drawBox(0, 0, w + 3, 8); u8g2.setDrawColor(1); u8g2.drawStr(1, 6, s);
  if (fbMode == 0) { u8g2.setDrawColor(0); u8g2.drawBox(8, 30, 48, 9); u8g2.setDrawColor(1); drawCentered(37, "R: FLAP"); }
  if (fbMode == 2) {
    u8g2.setDrawColor(0); u8g2.drawBox(4, 8, 56, 32); u8g2.setDrawColor(1); u8g2.drawFrame(4, 8, 56, 32);
    char b[16];
    drawCentered(16, "GAME OVER");
    snprintf(b, sizeof(b), "SCORE %d", fbScore); drawCentered(24, b);
    snprintf(b, sizeof(b), "BEST %d", fbBest);   drawCentered(31, b);
    drawCentered(38, "R:RETRY L:EXIT");
  }
}

static bool tryBrick(float fx, float fy) {
  int x = (int)fx, y = (int)fy;
  if (y < 9 || y >= 25 || x < 0 || x >= 64) return false;
  int r = (y - 9) / 4; if ((y - 9) % 4 == 3) return false;
  int c = x / 8;      if (x % 8 == 7) return false;
  if (!((brk[r] >> c) & 1)) return false;
  brk[r] &= ~(1 << c); bricksLeft--; bScore += 10 * (4 - r);
  return true;
}
static void breakerResetBall() { bx = bpx + PADDLE_W / 2 - 1; by = PADDLE_Y - 3; bvx = bvy = 0; bMode = 0; bModeAt = millis(); }
static void breakerNewGame() {
  for (auto& r : brk) r = 0xFF;
  bricksLeft = 32; bScore = 0; bLives = 3; bspd = 38; bpx = 25; breakerResetBall();
  gPressLatch[0] = gPressLatch[1] = false;
}
static void breakerUpdate(float dt) {
  if (bMode == 2) return;
  if (bStable[0]) bpx -= 62 * dt;
  if (bStable[1]) bpx += 62 * dt;
  if (bpx < 0) bpx = 0; if (bpx > 64 - PADDLE_W) bpx = 64 - PADDLE_W;
  if (bMode == 0) {
    bx = bpx + PADDLE_W / 2 - 1; by = PADDLE_Y - 3;
    if (millis() - bModeAt > 1200) {
      bMode = 1; bvx = (random(2) ? 1 : -1) * bspd * 0.45f; bvy = -sqrtf(bspd * bspd - bvx * bvx);
    }
    return;
  }
  float nx = bx + bvx * dt;
  if (tryBrick(nx + 1, by + 1)) bvx = -bvx; else bx = nx;
  float ny = by + bvy * dt;
  if (tryBrick(bx + 1, ny + 1)) bvy = -bvy; else by = ny;
  if (bx < 0) { bx = 0; bvx = fabsf(bvx); }
  if (bx > 62) { bx = 62; bvx = -fabsf(bvx); }
  if (by < 7) { by = 7; bvy = fabsf(bvy); }
  if (bvy > 0 && by + 2 >= PADDLE_Y && by <= PADDLE_Y + 1 && bx + 2 >= bpx && bx <= bpx + PADDLE_W) {
    float rel = ((bx + 1) - (bpx + PADDLE_W / 2.0f)) / (PADDLE_W / 2.0f + 1);
    if (rel > 1) rel = 1; if (rel < -1) rel = -1;
    bvx = bspd * 0.85f * rel; bvy = -sqrtf(bspd * bspd - bvx * bvx); by = PADDLE_Y - 3;
  }
  if (by >= 48) {
    if (--bLives <= 0) { bMode = 2; bModeAt = millis(); } else breakerResetBall();
  }
  if (bricksLeft <= 0) {
    bspd = min(bspd * 1.12f, 70.0f);
    for (auto& r : brk) r = 0xFF;
    bricksLeft = 32; breakerResetBall();
  }
}
static void renderBreaker() {
  char s[16]; snprintf(s, sizeof(s), "S:%d L:%d", bScore, bLives);
  u8g2.setFont(FONT_S); u8g2.drawStr(0, 6, s);
  for (int r = 0; r < 4; r++) for (int c = 0; c < 8; c++)
    if ((brk[r] >> c) & 1) u8g2.drawBox(c * 8, 9 + r * 4, 7, 3);
  u8g2.drawBox((int)bpx, PADDLE_Y, PADDLE_W, 2);
  u8g2.drawBox((int)bx, (int)by, 2, 2);
  if (bMode == 0) drawCentered(38, "READY");
  if (bMode == 2) {
    u8g2.setDrawColor(0); u8g2.drawBox(6, 14, 52, 24); u8g2.setDrawColor(1); u8g2.drawFrame(6, 14, 52, 24);
    drawCentered(22, "GAME OVER"); snprintf(s, sizeof(s), "SCORE %d", bScore); drawCentered(29, s);
    drawCentered(36, "ANY BUTTON");
  }
}

// ============================== RENDER DISPATCH =============================
static bool isAnimated() {
  switch (state) { case ST_SECRET: case ST_GAMES: case ST_KIOSK_MENU: return false; default: return true; }
}

static void render() {
  u8g2.clearBuffer();
  u8g2.setFontMode(1); u8g2.setDrawColor(1);
  char id[4];
  switch (state) {
    case ST_BOOT: {
      int h = (millis() - stateStart) / 8; if (h > 48) h = 48;
      u8g2.setClipWindow(0, 0, 64, h); drawLogo(); u8g2.setMaxClipWindow(); break; }
    case ST_ATTRACT:       renderAttract(); break;
    case ST_RENTAL_LIST:   renderRentalList(); break;
    case ST_RENTAL_DETAIL: renderRentalDetail(); break;
    case ST_PAYMENT:       renderPayment(); break;
    case ST_SECRET:  idStr2(id, sizeof(id)); drawList("SECRET", id, 4, secretSel, lblSecret); break;
    case ST_MESSAGES: idStr2(id, sizeof(id)); drawList("MESSAGES", id, msgCount + 1, msgSel, lblMsg); break;
    case ST_MSG_VIEW:      renderMsgView(); break;
    case ST_SCAN:          renderScan(); break;
    case ST_KIOSK_MENU: { char t[12]; snprintf(t, sizeof(t), "KIOSK %03u", (unsigned)kioskTarget);
      idStr2(id, sizeof(id)); drawList(t, id, 3, kioskSel, lblKiosk); break; }
    case ST_EDITOR:        renderEditor(); break;
    case ST_TX:            renderTx(); break;
    case ST_GAMES:   idStr2(id, sizeof(id)); drawList("GAMES", id, 3, gamesSel, lblGames); break;
    case ST_FLAPPY:        renderFlappy(); break;
    case ST_BREAKER:       renderBreaker(); break;
    case ST_ALERT:         renderAlert(); break;
  }
  u8g2.sendBuffer();      // full-buffer mode: one atomic frame push = no flicker
}

// ============================== EVENT HANDLING ==============================
static void goAlert() { alertSince = millis(); setState(ST_ALERT); }

static void editorEnter() {
  if (edTarget != kioskTarget) { edTarget = kioskTarget; edLen = 0; edText[0] = 0; }
  edSel = 0; setState(ST_EDITOR);
}

static void handleEvent(Ev ev) {
  lastInput = millis(); dirty = true;

  if (state == ST_ALERT) {                               // any button dismisses
    if (millis() - alertSince < 700) return;             // ignore a button that was already held
    if (alerts[0].kind == ALERT_MSG) {
      viewFrom = alerts[0].from; strlcpy(viewText, alerts[0].text, sizeof(viewText));
      storeMessage(viewFrom, viewText);                  // save to flash
      popAlert(); msgFromAlert = true; setState(ST_MSG_VIEW);
    } else {
      popAlert();
      if (alertCount) goAlert(); else setState(resumeState);
    }
    return;
  }

  if (ev == EV_BOTH) {                                   // secret menu, any time
    msgFromAlert = false;
    if (state == ST_SECRET) setState(ST_ATTRACT);
    else { secretSel = 0; selChangedAt = millis(); setState(ST_SECRET); }
    return;
  }

  switch (state) {
    case ST_BOOT: break;
    case ST_ATTRACT:
      rentalSel = 0; selChangedAt = millis(); setState(ST_RENTAL_LIST); break;
    case ST_RENTAL_LIST:
      if (ev == EV_L) moveSel(rentalSel, MOVIE_COUNT + 1);
      else if (ev == EV_R_LONG) { rentalSel = (rentalSel + MOVIE_COUNT) % (MOVIE_COUNT + 1); selChangedAt = millis(); }
      else if (ev == EV_R) {
        if (rentalSel >= (int)MOVIE_COUNT) setState(ST_ATTRACT);
        else { selChangedAt = millis(); setState(ST_RENTAL_DETAIL); }
      } else if (ev == EV_L_LONG) setState(ST_ATTRACT);
      break;
    case ST_RENTAL_DETAIL:
      if (ev == EV_L || ev == EV_L_LONG) { selChangedAt = millis(); setState(ST_RENTAL_LIST); }
      else if (ev == EV_R) setState(ST_PAYMENT);
      break;
    case ST_PAYMENT: break;
    case ST_SECRET:
      if (ev == EV_L) moveSel(secretSel, 4);
      else if (ev == EV_L_LONG) setState(ST_ATTRACT);
      else if (ev == EV_R) {
        if (secretSel == 0) { msgSel = msgCount ? msgCount - 1 : 0; selChangedAt = millis(); setState(ST_MESSAGES); }
        else if (secretSel == 1) { seenCount = 0; scanSel = 0; scanNextAt = 0; selChangedAt = millis(); setState(ST_SCAN); }
        else if (secretSel == 2) { gamesSel = 0; setState(ST_GAMES); }
        else setState(ST_ATTRACT);
      }
      break;
    case ST_MESSAGES:
      if (ev == EV_L) moveSel(msgSel, msgCount + 1);
      else if (ev == EV_L_LONG) setState(ST_SECRET);
      else if (ev == EV_R) {
        if (msgSel >= msgCount) setState(ST_SECRET);
        else {
          StoredMsg m; readStored(msgSel, m);
          viewIdx = msgSel; viewFrom = m.from; strlcpy(viewText, m.text, sizeof(viewText));
          msgFromAlert = false; setState(ST_MSG_VIEW);
        }
      }
      break;
    case ST_MSG_VIEW:
      if (msgFromAlert) {                                // dismiss alert-view
        msgFromAlert = false;
        if (alertCount) goAlert(); else setState(resumeState);
      } else if (ev == EV_R && msgCount) {               // next message
        viewIdx = (viewIdx + 1) % msgCount;
        StoredMsg m; readStored(viewIdx, m);
        viewFrom = m.from; strlcpy(viewText, m.text, sizeof(viewText));
      } else if (ev == EV_L || ev == EV_L_LONG) { msgSel = viewIdx; selChangedAt = millis(); setState(ST_MESSAGES); }
      break;
    case ST_SCAN:
      if (ev == EV_L) moveSel(scanSel, seenCount + 1);
      else if (ev == EV_L_LONG) setState(ST_SECRET);
      else if (ev == EV_R) {
        if (scanSel >= seenCount) setState(ST_SECRET);
        else { kioskTarget = seen[scanSel].id; kioskSel = 0; setState(ST_KIOSK_MENU); }
      }
      break;
    case ST_KIOSK_MENU:
      if (ev == EV_L) moveSel(kioskSel, 3);
      else if (ev == EV_L_LONG) setState(ST_SCAN);
      else if (ev == EV_R) {
        if (kioskSel == 0) editorEnter();
        else if (kioskSel == 1) { txStart(1, kioskTarget, ""); setState(ST_TX); }
        else setState(ST_SCAN);
      }
      break;
    case ST_EDITOR:
      if (ev == EV_L) edSel = (edSel + 1) % CHARSET_N;
      else if (ev == EV_L_LONG) { if (edLen < MSG_LEN) { edText[edLen++] = ' '; edText[edLen] = 0; } }
      else if (ev == EV_R) {
        char c = CHARSET[edSel];
        if (c == '<') { if (edLen) edText[--edLen] = 0; }
        else if (c == '^') edCaps = !edCaps;
        else if (c == '#') setState(ST_KIOSK_MENU);
        else if (edLen < MSG_LEN) { edText[edLen++] = tokenChar(edSel); edText[edLen] = 0; }
      } else if (ev == EV_R_LONG) {
        if (edLen == 0) { edNotice = "TYPE FIRST!"; edNoticeUntil = millis() + 1200; }
        else { txStart(0, edTarget, edText); setState(ST_TX); }
      }
      break;
    case ST_TX:
      if (!tx.active) setState(ST_KIOSK_MENU);           // result acknowledged
      break;
    case ST_GAMES:
      if (ev == EV_L) moveSel(gamesSel, 3);
      else if (ev == EV_L_LONG) setState(ST_SECRET);
      else if (ev == EV_R) {
        if (gamesSel == 0) { flappyReset(); setState(ST_FLAPPY); }
        else if (gamesSel == 1) { breakerNewGame(); setState(ST_BREAKER); }
        else setState(ST_SECRET);
      }
      break;
    case ST_FLAPPY:
      if (ev == EV_L || ev == EV_L_LONG) setState(ST_GAMES);
      break;
    case ST_BREAKER:
      if (bMode == 2 && millis() - bModeAt > 600) setState(ST_GAMES);
      break;
    default: break;
  }
}

// ============================== SETUP / LOOP ================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);                 // logo LED on by default

  loadSettings();
  randomSeed(micros() ^ ESP.getChipId());
  txSeq = random(256);

  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setFontMode(1);

  radioInit();                                 // ESP-NOW runs in the background from here on
  Serial.printf("\nRIPBOX kiosk ID %03u  radio:%s\n", (unsigned)myId, radioOk ? "ok" : "FAIL");

  setState(ST_BOOT);
  lastInput = millis();
}

void loop() {
  uint32_t now = millis();

  Ev ev = readButtons();
  serialService();
  rxService();
  txQueueService();
  txService();

  // ESP-NOW discovery while the scan menu is open
  if (state == ST_SCAN) {
    pruneSeen();
    if ((int32_t)(now - scanNextAt) >= 0) { sendSimple(PKT_SCAN, 0, 0); scanNextAt = now + 1500; }
    if (scanSel > seenCount) scanSel = seenCount;
  }

  // incoming alerts interrupt whatever is on screen
  if (alertCount > 0 && state != ST_ALERT && !(state == ST_MSG_VIEW && msgFromAlert)) {
    resumeState = state;
    goAlert();
  }

  if (ev != EV_NONE) handleEvent(ev);

  // timed transitions
  if (state == ST_BOOT && now - stateStart > 2600) setState(ST_ATTRACT);
  if (state == ST_PAYMENT && now - stateStart > PAY_MS) setState(ST_ATTRACT);
  if ((state == ST_RENTAL_LIST || state == ST_RENTAL_DETAIL) && now - lastInput > IDLE_RENTAL_MS) setState(ST_ATTRACT);

  // logo LED: always on, except flashing during an alert
  digitalWrite(PIN_LED, (state == ST_ALERT) ? ((now / 130) & 1) : HIGH);

  // fixed-rate frame tick
  if (now - lastFrame >= FRAME_MS) {
    float dt = (now - lastFrame) / 1000.0f; if (dt > 0.05f) dt = 0.05f;
    lastFrame = now;
    if (state == ST_FLAPPY) flappyUpdate(dt);
    if (state == ST_BREAKER) breakerUpdate(dt);
    if (dirty || isAnimated()) { render(); dirty = false; }
  }
  delay(1);
}
