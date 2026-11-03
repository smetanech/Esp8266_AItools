/*
 * ============================================================
 *  ESP8266 AI Terminal v8.6
 * ============================================================
 *  Умный OLED-терминал на ESP8266:
 *   - меню с AI-чатом (ответы получает через ретранслятор)
 *   - часы (NTP), заметка, карты, калькулятор
 *   - сообщения, заметки, быстрые шаблоны
 *   - три игры: Pong, Dodger, Dino (как Chrome Dino)
 *   - встроенный веб-сервер с панелью управления
 *
 *  Железо:
 *   - OLED SSD1306 128x64, 4-проводный SPI (GPIO 14/13/15/4)
 *   - кнопки: NEXT (D4), SEND (D6)
 *
 *  Прошивка: Arduino IDE с пакетом ESP8266 (NodeMCU 1.0).
 *  Питание логики дисплея и т.п. — см. README.
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <Arduino.h>
#include <U8g2lib.h>
#include <FS.h>
#include <time.h>
#include <ArduinoJson.h>

const char* FW_VERSION = "ESP8266 MOD v8.6";

const int PHONE_RELAY_PORT = 8000;
// ======================================================================
//  ЛИЧНЫЕ НАСТРОЙКИ — заполни перед прошивкой / удали из репозитория.
// ======================================================================
const char* PHONE_RELAY_FALLBACK = "192.168.0.100";   // IP ретранслятора (телефона)
const char* FALLBACK_SSID = "YOUR_WIFI_SSID";         // запасная сеть WiFi
const char* FALLBACK_PASS = "";                        // пароль от неё (пусто = open)

String PHONE_RELAY_HOST = PHONE_RELAY_FALLBACK;

// Дисплей SSD1306 128x64 по SPI: (SCL, SDA, CS, DC, RST). Выставь под свою разводку.
U8G2_SSD1306_128X64_NONAME_F_4W_SW_SPI u8g2(U8G2_R0, 14, 13, 15, 4, 0);
// Кнопки на D4 (NEXT) и D6 (SEND), подтяжка вверх — замыкание на GND.
#define BTN_NEXT D4
#define BTN_SEND D6
ESP8266WebServer server(80);

#define FONT_MENU u8g2_font_6x10_tf
#define FONT_HUGE u8g2_font_7x14_tf
#define FONT_TINY u8g2_font_5x7_tf
#define FONT_BIG  u8g2_font_8x13_tf
#define SCREEN_W  128
#define SCREEN_H  64
#define CHARS_PER_LINE  21

#define MAX_PROMPTS   20
#define MAX_PROMPT_LEN 60
#define MAX_MESSAGES  30
#define MAX_MSG_LEN   180
#define MAX_NOTES     25
#define MAX_NOTE_LEN  180
#define MAX_AI_QUEUE  5
#define MAX_MAPS      25
#define MAX_WIFI_NETWORKS 12
#define MAX_LINES     100
#define MAX_LINE_LEN  100
#define MAX_TEMPLATES 8
#define MAX_NOTIFICATIONS 5
#define MAX_MENU_ITEMS 10
#define LOG_BUFFER_SIZE 25
#define DEBOUNCE_MS 180
#define HOLD_MS     600

enum State {
  STATE_SPLASH, STATE_MENU, STATE_STATS, STATE_AI_LIST, STATE_AI_THINKING, STATE_AI_RESPONSE,
  STATE_TIME, STATE_NOTICE, STATE_MAP_LIST, STATE_MAP_VIEW,
  STATE_SETTINGS, STATE_SETUP_WIFI_LIST, STATE_SETUP_WIFI_PASS, STATE_SETUP_WIFI_CONNECTING,
  STATE_SETUP_ORDER, STATE_SETUP_HELP, STATE_SETUP_BRIGHTNESS, STATE_SETUP_NOTIFY,
  STATE_SETUP_SLEEP, STATE_SETUP_SYNC, STATE_SYNC_STATUS, STATE_SYNC_NOW,
  STATE_MSG_MENU, STATE_MSG_CHAT, STATE_MSG_CHAT_VIEW, STATE_MSG_TEMPLATES,
  STATE_MSG_CUSTOM, STATE_MSG_CUSTOM_LETTER, STATE_CALCULATOR,
  STATE_NOTES_MENU, STATE_NOTES_ADD, STATE_NOTES_ADD_LETTER, STATE_NOTES_HISTORY, STATE_NOTES_VIEW,
  STATE_GAMES_MENU, STATE_GAME_PONG, STATE_GAME_PONG_PAUSE, STATE_GAME_PONG_SETTINGS,
  STATE_GAME_DODGER, STATE_GAME_DODGER_PAUSE,
  STATE_GAME_DINO, STATE_GAME_DINO_PAUSE
};
enum MyWiFiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };

State state = STATE_SPLASH;
MyWiFiState wifiState = WIFI_IDLE;
unsigned long wifiRetryAt = 0;
const unsigned long WIFI_RETRY_DELAY = 8000;
String currentSSID = "";
bool wifiConnected = false;
bool syncEnabled = true;
bool syncInProgress = false;
bool lastSyncSuccess = false;
unsigned long lastSyncTime = 0;
unsigned long lastSyncCheck = 0;
const unsigned long SYNC_INTERVAL = 5000;
bool inSleepMode = false;
bool timeSynced = false;
unsigned long lastBtnTime = 0;
int responsePage = 0;
int totalResponsePages = 1;
String responseText = "";
int wrappedLineCount = 0;
char wrappedLines[MAX_LINES][MAX_LINE_LEN];
unsigned long splashStartTime = 0;
const unsigned long SPLASH_DURATION = 3000;

bool nextIsHeld = false;
bool sendIsHeld = false;

String prompts[MAX_PROMPTS]; int promptCount = 0, currentItem = 0;
String messages[MAX_MESSAGES]; bool messagesFromESP[MAX_MESSAGES]; unsigned long msgTimes[MAX_MESSAGES]; int messagesCount = 0, currentMsg = 0;
String templates[MAX_TEMPLATES]; int templateCount = 0, currentTemplate = 0;
String notes[MAX_NOTES]; int notesCount = 0, currentNote = 0, notesMenuItem = 0;
String noteText = ""; int noteKeyboardGroup = 0, noteKeyboardLetter = 0;
String customMessage = ""; int keyboardGroup = 0, keyboardLetter = 0;
String noticeText = "No notice"; int noticePage = 0;
String mapNames[MAX_MAPS]; int mapCount = 0, currentMap = 0;
uint8_t currentMapData[1024]; bool mapLoaded = false;
struct Notification { String text; unsigned long time; bool active; };
Notification notifications[MAX_NOTIFICATIONS];
String logBuffer[LOG_BUFFER_SIZE]; int logIndex = 0, logCount = 0;
struct AIRequest { String prompt, response; bool responded; unsigned long time; };
AIRequest aiQueue[MAX_AI_QUEUE]; int aiQueueCount = 0;

const char LETTER_GROUPS[][6] = { {'A','B','C','D','E','\0'}, {'F','G','H','I','J','\0'}, {'K','L','M','N','O','\0'}, {'P','Q','R','S','T','\0'}, {'U','V','W','X','Y','\0'} };
const int LETTER_GROUPS_COUNT = 5;
const char NOTE_DIGITS_GROUP[] = "0123456789";

const char* DEFAULT_MENU_ITEMS[] = { "AI Chat", "Time", "Notice", "Maps", "Calc", "Msg", "Notes", "Games" };
String menuOrder[MAX_MENU_ITEMS]; int menuCount = 0, menuItem = 0;
int settingsItem = 0;
bool notificationsEnabled = true;
int brightnessLevel = 100;
bool sleepEnabled = true;
String wifiNetworks[MAX_WIFI_NETWORKS]; bool wifiNetworkOpen[MAX_WIFI_NETWORKS];
int wifiNetworkCount = 0, wifiSelectedNet = 0;
String wifiPassword = ""; int keyboardDigit = 1;

enum CalcState { CALC_DIGIT, CALC_OP, CALC_RESULT };
const char CALC_DIGITS[] = "0123456789"; const char CALC_OPS[] = "+-*/";
CalcState calcState = CALC_DIGIT; int calcDigitOrOp = 0; String calcExpr = ""; double calcResult = 0;

int pongPaddleLeftY=24, pongPaddleRightY=24, pongBallX=64, pongBallY=32, pongBallDX=1, pongBallDY=1, pongScore=0, pongAILevel=1, pongSpeed=1;
unsigned long pongLastUpdate=0; bool pongGameOver=false; int pongPauseItem=0;
const int DODGER_MAX_OBSTACLES=5; int dodgerX=60, dodgerY=56, dodgerObstacles[DODGER_MAX_OBSTACLES][2], dodgerScore=0, dodgerUpdateInterval=100, dodgerObstacleSpeed=1, dodgerPauseItem=0;
unsigned long dodgerLastUpdate=0; bool dodgerGameOver=false;
int gamesMenuItem = 0;

const uint8_t DINO_RUN1[12][12] = {
  {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,1,1},{0,0,0,0,0,0,0,0,0,1,1,1},
  {0,0,0,0,0,0,0,0,1,1,1,0},{0,0,0,0,0,0,0,1,1,1,0,0},{0,0,0,0,0,0,1,1,1,0,0,0},
  {0,1,1,1,1,1,1,1,1,1,0,0},{1,1,0,1,1,1,1,1,0,0,0,0},{1,1,1,1,1,1,1,0,0,0,0,0},
  {1,1,1,1,1,1,0,0,0,0,0,0},{0,1,1,1,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0}
};
const uint8_t DINO_RUN2[12][12] = {
  {0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,1,1},{0,0,0,0,0,0,0,0,0,1,1,1},
  {0,0,0,0,0,0,0,0,1,1,1,0},{0,0,0,0,0,0,0,1,1,1,0,0},{0,0,0,0,0,0,1,1,1,0,0,0},
  {0,1,1,1,1,1,1,1,1,1,0,0},{1,1,0,1,1,1,1,1,0,0,0,0},{1,1,1,1,1,1,1,0,1,0,0,0},
  {1,1,1,1,1,1,0,0,0,0,0,0},{0,1,1,1,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0}
};
const uint8_t DINO_DUCK[8][14] = {
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  {0,0,0,0,0,0,0,0,0,0,1,1,1,1},{0,0,0,0,0,0,0,1,1,1,1,1,1,0},
  {1,1,1,1,1,1,1,1,1,1,1,0,0,0},{1,1,1,1,1,1,1,1,1,1,0,0,0,0},
  {1,1,1,1,1,1,1,1,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};
const uint8_t CACTUS_SMALL[8][6] = {
  {0,0,1,1,0,0},{0,0,1,1,0,0},{0,1,1,1,1,0},{0,1,0,0,1,0},
  {0,1,0,0,1,0},{0,1,0,0,1,0},{0,1,0,0,1,0},{0,1,0,0,1,0}
};
const uint8_t CACTUS_BIG[12][6] = {
  {0,0,0,1,0,0},{0,0,0,1,0,0},{0,1,1,1,1,0},{0,1,1,1,1,0},
  {0,1,0,1,0,0},{0,1,0,1,0,0},{0,1,0,1,0,0},{0,1,0,1,0,0},
  {0,1,0,1,0,0},{0,1,0,1,0,0},{0,1,0,1,0,0},{0,1,0,1,0,0}
};
const uint8_t BIRD[6][8] = {
  {0,0,0,0,1,1,0,0},{0,0,0,1,1,1,1,0},{0,1,1,1,1,1,0,0},
  {0,1,1,1,1,1,0,0},{0,0,0,1,1,1,1,0},{0,0,0,0,1,1,0,0}
};

#define DINO_W 12
#define DINO_H 12
#define DUCK_W 14
#define DUCK_H 8
#define CACTUS_S_W 6
#define CACTUS_S_H 8
#define CACTUS_B_W 6
#define CACTUS_B_H 12
#define BIRD_W 8
#define BIRD_H 6

int dinoX = 20, dinoY = 40;
int dinoVelY = 0;
bool dinoJumping = false;
bool dinoOnGround = true;
int dinoScore = 0;
int dinoHighScore = 0;
unsigned long dinoLastUpdate = 0;
bool dinoGameOver = false;
int dinoRunFrame = 0;
unsigned long dinoRunTimer = 0;

const int MAX_DINO_OBST = 4;
int dinoObsX[MAX_DINO_OBST];
int dinoObsY[MAX_DINO_OBST];
int dinoObsType[MAX_DINO_OBST];
int dinoObsCount = 0;
const int DINO_UPDATE_MS = 50;
int dinoPauseItem = 0;
unsigned long dinoSpeedUp = 0;
int dinoCurrentSpeed = 4;
const int DINO_GROUND_Y = 52;

const char THINK_OPEN[] = {'<','t','h','i','n','k','>','\0'};
const char THINK_CLOSE[] = {'<','/','t','h','i','n','k','>','\0'};

// ========== FORWARD DECLARATIONS ==========
bool updPrompt(int i, const String& t);

// ========== LOGGING ==========
void addLog(const String& msg) {
  logBuffer[logIndex] = msg;
  logIndex = (logIndex + 1) % LOG_BUFFER_SIZE;
  if (logCount < LOG_BUFFER_SIZE) logCount++;
  Serial.println(msg);
}
void addNotification(const String& text) {
  if (!notificationsEnabled || inSleepMode) return;
  for (int i = MAX_NOTIFICATIONS - 1; i > 0; i--) notifications[i] = notifications[i - 1];
  notifications[0].text = text; notifications[0].time = millis(); notifications[0].active = true;
}
void updateNotifications() {
  for (int i = 0; i < MAX_NOTIFICATIONS; i++)
    if (notifications[i].active && millis() - notifications[i].time > 4000) notifications[i].active = false;
}
void drawNotifications() {
  if (!notificationsEnabled || inSleepMode) return;
  int y = 0;
  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    if (!notifications[i].active) continue;
    unsigned long age = millis() - notifications[i].time;
    int vy = y;
    if (age < 200) vy = y - (10 - (int)(age / 20));
    u8g2.setFont(FONT_TINY); u8g2.setDrawColor(1); u8g2.drawBox(0, vy, SCREEN_W, 11);
    u8g2.setDrawColor(0); u8g2.setCursor(4, vy + 8); u8g2.print(notifications[i].text);
    u8g2.setDrawColor(1); y += 12; if (y > 30) break;
  }
}

int utf8CharLen(uint8_t c) { if (c<0x80) return 1; if (c<0xC0) return 1; if (c<0xE0) return 2; if (c<0xF0) return 3; return 4; }
int wrapText(const String& text) {
  int lc = 0, pos = 0, len = text.length();
  while (pos < len && lc < MAX_LINES) {
    int cl = 0, bl = 0;
    while (pos < len && cl < CHARS_PER_LINE) {
      if (text[pos] == '\n') { pos++; break; }
      int we = pos, wc = 0;
      while (we < len && text[we] != ' ' && text[we] != '\n') { we += utf8CharLen(text[we]); wc++; }
      int need = wc + (cl > 0 ? 1 : 0);
      if (cl + need <= CHARS_PER_LINE) {
        if (cl > 0) { wrappedLines[lc][bl++] = ' '; cl++; }
        int wb = we - pos;
        for (int i = 0; i < wb; i++) if (bl < MAX_LINE_LEN - 1) wrappedLines[lc][bl++] = text[pos + i];
        cl += wc; pos = we; if (pos < len && text[pos] == ' ') pos++;
      } else {
        if (cl == 0) {
          int cf = min(wc, CHARS_PER_LINE), ci = 0, bi = 0;
          while (ci < cf && pos + bi < len) {
            int clen = utf8CharLen(text[pos + bi]);
            for (int i = 0; i < clen; i++) if (bl < MAX_LINE_LEN - 1) wrappedLines[lc][bl++] = text[pos + bi + i];
            bi += clen; ci++;
          } pos += bi;
        } break;
      }
    }
    wrappedLines[lc][bl] = '\0'; lc++;
    while (pos < len && text[pos] == '\n') pos++;
  }
  return lc;
}
String extractJsonField(const String& json, const String& field) {
  String k = String("\"") + field + "\":\""; int s = json.indexOf(k); if (s < 0) return "";
  s += k.length(); int e = s;
  while (e < json.length()) {
    if (json[e] == '\\' && e + 1 < json.length()) {
      char n = json[e + 1]; if (n == 'u' && e + 5 < json.length()) e += 6; else e += 2;
    } else if (json[e] == '"') break; else e++;
  }
  if (e >= json.length()) return "";
  String r = json.substring(s, e);
  r.replace("\\n","\n"); r.replace("\\\"","\""); r.replace("\\\\","\\"); return r;
}
String stripThinking(const String& t) {
  int s = t.indexOf(THINK_OPEN); int e = t.indexOf(THINK_CLOSE);
  if (s >= 0 && e > s) { String r = t.substring(e + 7); r.trim(); return r; } return t;
}
String urlDecode(const String& in) {
  String out = ""; for (int i = 0; i < in.length(); i++) {
    char c = in[i]; if (c == '+') out += ' ';
    else if (c == '%' && i + 2 < in.length()) { char h[3] = {in[i+1],in[i+2],0}; out += (char)strtoul(h,NULL,16); i += 2; }
    else out += c;
  } return out;
}
String formatTime(struct tm* t) { if (!t) return "--:--"; char b[9]; sprintf(b,"%02d:%02d:%02d",t->tm_hour,t->tm_min,t->tm_sec); return String(b); }
void syncTime() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(10*3600, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
    for (int i = 0; i < 10; i++) { time_t n = time(nullptr); if (n > 1000000) { timeSynced = true; break; } delay(100); ESP.wdtFeed(); }
  }
}

void saveSettings() {
  File f = SPIFFS.open("/settings.txt", "w"); if (!f) return;
  f.println(notificationsEnabled ? "1" : "0");
  f.println(brightnessLevel);
  f.println(sleepEnabled ? "1" : "0");
  f.println(menuCount);
  for (int i = 0; i < menuCount; i++) f.println(menuOrder[i]);
  f.close();
}
void loadSettings() {
  if (!SPIFFS.exists("/settings.txt")) return;
  File f = SPIFFS.open("/settings.txt", "r"); if (!f) return;
  notificationsEnabled = (f.parseInt() == 1); f.readStringUntil('\n');
  brightnessLevel = f.parseInt(); f.readStringUntil('\n');
  sleepEnabled = (f.parseInt() == 1); f.readStringUntil('\n');
  int c = f.parseInt(); f.readStringUntil('\n');
  if (c > 0 && c <= MAX_MENU_ITEMS) {
    menuCount = c;
    for (int i = 0; i < menuCount; i++) { menuOrder[i] = f.readStringUntil('\n'); menuOrder[i].trim(); }
  }
  f.close();
}
void savePrompts() {
  File f = SPIFFS.open("/prompts.txt", "w"); if (!f) return;
  f.println(promptCount);
  for (int i = 0; i < promptCount; i++) f.println(prompts[i]);
  f.close();
}
void loadPrompts() {
  if (!SPIFFS.exists("/prompts.txt")) return;
  File f = SPIFFS.open("/prompts.txt", "r"); if (!f) return;
  promptCount = f.parseInt();
  if (promptCount > MAX_PROMPTS) promptCount = MAX_PROMPTS;
  f.readStringUntil('\n');
  for (int i = 0; i < promptCount; i++) { prompts[i] = f.readStringUntil('\n'); prompts[i].trim(); }
  f.close();
}
void saveNotes() {
  File f = SPIFFS.open("/notes.txt", "w"); if (!f) return;
  f.println(notesCount);
  for (int i = 0; i < notesCount; i++) f.println(notes[i]);
  f.close();
}
void loadNotes() {
  if (!SPIFFS.exists("/notes.txt")) return;
  File f = SPIFFS.open("/notes.txt", "r"); if (!f) return;
  notesCount = f.parseInt();
  if (notesCount > MAX_NOTES) notesCount = MAX_NOTES;
  f.readStringUntil('\n');
  for (int i = 0; i < notesCount; i++) { notes[i] = f.readStringUntil('\n'); notes[i].trim(); }
  f.close();
}
void saveMessages() {
  File f = SPIFFS.open("/messages.txt", "w"); if (!f) return;
  f.println(messagesCount);
  for (int i = 0; i < messagesCount; i++) {
    f.println(messagesFromESP[i] ? "1" : "0");
    f.println(msgTimes[i]);
    f.println(messages[i]);
  }
  f.close();
}
void loadMessages() {
  if (!SPIFFS.exists("/messages.txt")) return;
  File f = SPIFFS.open("/messages.txt", "r"); if (!f) return;
  messagesCount = f.parseInt();
  if (messagesCount > MAX_MESSAGES) messagesCount = MAX_MESSAGES;
  f.readStringUntil('\n');
  for (int i = 0; i < messagesCount; i++) {
    messagesFromESP[i] = (f.parseInt() == 1); f.readStringUntil('\n');
    msgTimes[i] = f.parseInt(); f.readStringUntil('\n');
    messages[i] = f.readStringUntil('\n'); messages[i].trim();
  }
  f.close();
}
void loadMapList() {
  mapCount = 0;
  Dir dir = SPIFFS.openDir("/maps");
  while (dir.next() && mapCount < MAX_MAPS) {
    String n = dir.fileName(); n.replace("/maps/",""); n.replace(".bin","");
    if (n.length() > 0 && n.length() < 24) mapNames[mapCount++] = n;
  }
}

void detectPhoneIP() {
  if (WiFi.status() != WL_CONNECTED) return;
  IPAddress gw = WiFi.gatewayIP();
  if (gw[0] != 0 && gw[0] != 255) {
    PHONE_RELAY_HOST = gw.toString();
    addLog(String("[WiFi] Phone IP: ") + PHONE_RELAY_HOST);
  }
}

void startWifiAttempt(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  addLog(String("[WiFi] Try: ") + ssid);
  WiFi.disconnect(true); delay(50);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  wifiState = WIFI_CONNECTING; currentSSID = ssid;
}
// Автоподключение к WiFi: сначала к точке доступа телефона (реле),
// не вышло — к запасной домашней сети. Чередуем, пока не подключимся.
void wifiLoop() {
  static unsigned long lastCheck = 0;
  if (millis() - lastCheck < 500) return;
  lastCheck = millis();
  switch (wifiState) {
    case WIFI_CONNECTING: {
      if (WiFi.status() == WL_CONNECTED) {
        wifiState = WIFI_CONNECTED; wifiConnected = true;
        addLog(String("[WiFi] OK ") + WiFi.localIP().toString());
        addNotification("WiFi OK");
        detectPhoneIP();
        ESP.wdtFeed();
        if (!timeSynced) syncTime();
        return;
      }
      static unsigned long connectStart = 0;
      if (connectStart == 0) connectStart = millis();
      if (millis() - connectStart > 15000) {
        connectStart = 0; addLog("[WiFi] Timeout");
        wifiState = WIFI_FAILED; WiFi.disconnect();
      }
      break;
    }
    case WIFI_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        addLog("[WiFi] Dropped");
        wifiState = WIFI_FAILED; wifiConnected = false;
        wifiRetryAt = millis() + 1000;
      }
      break;
    case WIFI_FAILED: case WIFI_IDLE:
      if (millis() < wifiRetryAt) return;
      static bool useFallback = false;
      if (!useFallback) { startWifiAttempt(PHONE_RELAY_FALLBACK, ""); useFallback = true; }
      else { startWifiAttempt(FALLBACK_SSID, FALLBACK_PASS); useFallback = false; }
      wifiRetryAt = millis() + WIFI_RETRY_DELAY;
      break;
  }
}

String httpRequest(const String& url, const String& method, const String& body, int timeout = 8000) {
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClient client; HTTPClient http;
  if (!http.begin(client, url)) return "";
  http.setTimeout(timeout);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP8266");
  int code = (method == "POST") ? http.POST(body) : http.GET();
  String resp = "";
  if (code > 0) resp = http.getString();
  http.end();
  return (code == 200) ? resp : "";
}
// Полная синхронизация с ретранслятором: отправляем туда свои данные
// (промпты, заметки, сообщения, заметку) и тут же забираем обновлённые.
// Вызывается периодически из loop(), пока WiFi подключён.
bool doSync() {
  if (syncInProgress || !wifiConnected) return false;
  syncInProgress = true; ESP.wdtFeed();
  StaticJsonDocument<2048> doc;
  doc["device_id"] = WiFi.macAddress();
  JsonArray pa = doc.createNestedArray("prompts");
  for (int i = 0; i < promptCount; i++) pa.add(prompts[i]);
  JsonArray na = doc.createNestedArray("notes");
  for (int i = 0; i < notesCount; i++) na.add(notes[i]);
  JsonArray ma = doc.createNestedArray("messages");
  for (int i = 0; i < messagesCount; i++) {
    JsonObject o = ma.createNestedObject();
    o["text"] = messages[i]; o["fromESP"] = messagesFromESP[i]; o["time"] = msgTimes[i];
  }
  doc["notice"] = noticeText;
  String body; serializeJson(doc, body);
  String url = String("http://") + PHONE_RELAY_HOST + ":" + PHONE_RELAY_PORT + "/api/state";
  httpRequest(url, "POST", body, 5000);
  delay(50); ESP.wdtFeed();
  String resp = httpRequest(url, "GET", "", 5000);
  if (resp.length() > 0) {
    StaticJsonDocument<4096> rd;
    if (!deserializeJson(rd, resp)) {
      if (rd.containsKey("prompts")) {
        JsonArray a = rd["prompts"]; promptCount = 0;
        for (JsonVariant v : a) if (promptCount < MAX_PROMPTS) prompts[promptCount++] = v.as<String>();
        savePrompts();
      }
      if (rd.containsKey("notes")) {
        JsonArray a = rd["notes"]; notesCount = 0;
        for (JsonVariant v : a) if (notesCount < MAX_NOTES) notes[notesCount++] = v.as<String>();
        saveNotes();
      }
      if (rd.containsKey("messages")) {
        JsonArray a = rd["messages"]; messagesCount = 0;
        for (JsonObject o : a) if (messagesCount < MAX_MESSAGES) {
          messages[messagesCount] = o["text"].as<String>();
          messagesFromESP[messagesCount] = o["fromESP"].as<bool>();
          msgTimes[messagesCount] = o["time"].as<unsigned long>();
          messagesCount++;
        }
        saveMessages();
      }
      if (rd.containsKey("notice")) noticeText = rd["notice"].as<String>();
      if (rd.containsKey("ai_responses")) {
        JsonArray a = rd["ai_responses"];
        for (JsonObject o : a) {
          String p = o["prompt"].as<String>(), r = o["response"].as<String>();
          bool found = false;
          for (int i = 0; i < aiQueueCount; i++) if (aiQueue[i].prompt == p && aiQueue[i].responded) { found = true; break; }
          if (!found && aiQueueCount < MAX_AI_QUEUE) {
            aiQueue[aiQueueCount++] = {p, r, true, millis()};
            addNotification("AI reply");
          }
        }
      }
      lastSyncSuccess = true; lastSyncTime = millis();
    }
  }
  syncInProgress = false; ESP.wdtFeed();
  return lastSyncSuccess;
}
// Отправка одного промпта в AI. Ответ ждём долго (до 3 минут) —
// Ollama на телефоне/ПК под маленькой моделью может думать не спеша.
String askAI(const String& prompt) {
  if (!wifiConnected) return String("Err: WiFi off");
  StaticJsonDocument<384> doc;
  doc["prompt"] = prompt; doc["device_id"] = WiFi.macAddress();
  String body; serializeJson(doc, body);
  String url = String("http://") + PHONE_RELAY_HOST + ":" + PHONE_RELAY_PORT + "/api/ai";
  String resp = httpRequest(url, "POST", body, 180000);
  if (resp.length() == 0) return String("Err: timeout");
  StaticJsonDocument<4096> rd;
  if (deserializeJson(rd, resp)) return String("Err: parse");
  String r = rd["response"].as<String>();
  return r.length() ? r : String("Err: empty");
}

// Порядок меню из настроек подставляем только если они ещё не загружены,
// иначе пользовательская расстановка пунктов стиралась бы при каждом включении.
void initMenuOrder() { if (menuCount > 0) return; for (int i = 0; i < 8; i++) menuOrder[i] = DEFAULT_MENU_ITEMS[i]; menuCount = 8; }
// Стартовые промпты заполняем только если в SPIFFS ещё ничего нет.
// Иначе пользовательские промпты затрутся при каждом включении.
void initPrompts() {
  if (promptCount > 0) return;
  prompts[0]="Tell me a joke"; prompts[1]="Explain quantum physics"; prompts[2]="Write a poem";
  prompts[3]="What is AI?"; prompts[4]="Motivational quote"; prompts[5]="Interesting fact";
  prompts[6]="How to code?"; prompts[7]="Meaning of life";
  promptCount = 8; savePrompts();
}
void initTemplates() {
  templates[0]="I'm busy"; templates[1]="On my way"; templates[2]="Call you later";
  templates[3]="Thanks!"; templates[4]="OK"; templateCount = 5;
}
bool addPrompt(const String& t) {
  if (promptCount >= MAX_PROMPTS || t.length() == 0 || t.length() > MAX_PROMPT_LEN) return false;
  prompts[promptCount++] = t; savePrompts(); return true;
}
bool updPrompt(int i, const String& t) {
  if (i < 0 || i >= promptCount || t.length() == 0 || t.length() > MAX_PROMPT_LEN) return false;
  prompts[i] = t; savePrompts(); return true;
}
bool delPrompt(int i) {
  if (i < 0 || i >= promptCount) return false;
  for (int k = i; k < promptCount - 1; k++) prompts[k] = prompts[k + 1];
  promptCount--; savePrompts(); return true;
}
void addMessage(const String& t, bool f) {
  if (t.length() == 0 || t.length() > MAX_MSG_LEN) return;
  if (messagesCount >= MAX_MESSAGES) {
    for (int i = 0; i < MAX_MESSAGES - 1; i++) { messages[i] = messages[i + 1]; messagesFromESP[i] = messagesFromESP[i + 1]; msgTimes[i] = msgTimes[i + 1]; }
    messagesCount = MAX_MESSAGES - 1;
  }
  messages[messagesCount] = t; messagesFromESP[messagesCount] = f; msgTimes[messagesCount] = millis();
  messagesCount++; saveMessages();
}
void clearMessages() { messagesCount = 0; if (SPIFFS.exists("/messages.txt")) SPIFFS.remove("/messages.txt"); }
void addNote(const String& t) {
  if (t.length() == 0 || t.length() > MAX_NOTE_LEN) return;
  if (notesCount >= MAX_NOTES) {
    for (int i = 0; i < MAX_NOTES - 1; i++) notes[i] = notes[i + 1];
    notesCount = MAX_NOTES - 1;
  }
  notes[notesCount++] = t; saveNotes();
}
bool updNote(int i, const String& t) {
  if (i < 0 || i >= notesCount || t.length() == 0 || t.length() > MAX_NOTE_LEN) return false;
  notes[i] = t; saveNotes(); return true;
}
bool delNote(int i) {
  if (i < 0 || i >= notesCount) return false;
  for (int k = i; k < notesCount - 1; k++) notes[k] = notes[k + 1];
  notesCount--; saveNotes(); return true;
}
void clearNotes() { notesCount = 0; if (SPIFFS.exists("/notes.txt")) SPIFFS.remove("/notes.txt"); }

void calcReset() { calcState = CALC_DIGIT; calcDigitOrOp = 0; calcExpr = ""; calcResult = 0; }
String formatCalc(double n) {
  double r = round(n * 1000) / 1000;
  if (r == (int64_t)r) return String((int64_t)r);
  String s = String(r, 3); while (s.endsWith("0")) s.remove(s.length()-1); if (s.endsWith(".")) s.remove(s.length()-1); return s;
}
void calcCycle() { if (calcState == CALC_DIGIT) calcDigitOrOp = (calcDigitOrOp + 1) % 10; else if (calcState == CALC_OP) calcDigitOrOp = (calcDigitOrOp + 1) % 4; }
void calcAdd() {
  if (calcState == CALC_RESULT) { calcExpr = formatCalc(calcResult); calcState = CALC_OP; calcDigitOrOp = 0; return; }
  if (calcState == CALC_DIGIT) { calcExpr += String(CALC_DIGITS[calcDigitOrOp]); calcState = CALC_OP; }
  else if (calcState == CALC_OP) {
    if (calcExpr.length() == 0) return; char l = calcExpr[calcExpr.length()-1];
    if (l=='+'||l=='-'||l=='*'||l=='/') calcExpr[calcExpr.length()-1] = CALC_OPS[calcDigitOrOp]; else calcExpr += String(CALC_OPS[calcDigitOrOp]);
    calcState = CALC_DIGIT;
  }
  calcDigitOrOp = 0;
}
void calcCompute() {
  if (calcExpr.length() == 0) return;
  while (calcExpr.length() > 0) { char l = calcExpr[calcExpr.length()-1]; if (l=='+'||l=='-'||l=='*'||l=='/') calcExpr.remove(calcExpr.length()-1); else break; }
  if (calcExpr.length() == 0) return;
  calcResult = calcEval(calcExpr); calcState = CALC_RESULT;
}
double calcEval(const String& expr) {
  double n[20]; char o[20]; int nc=0,oc=0; String cn="";
  for (int i = 0; i < expr.length(); i++) { char c = expr[i]; if (c=='+'||c=='-'||c=='*'||c=='/') { if (cn.length()>0) { n[nc++]=cn.toFloat(); cn=""; } o[oc++]=c; } else cn+=c; }
  if (cn.length()>0) n[nc++]=cn.toFloat();
  for (int i = 0; i < oc; i++) { if (o[i]=='*'||o[i]=='/') { double r = (o[i]=='*') ? n[i]*n[i+1] : (n[i+1]!=0 ? n[i]/n[i+1] : 0); n[i]=r; for (int j=i+1;j<nc-1;j++) n[j]=n[j+1]; for (int j=i;j<oc-1;j++) o[j]=o[j+1]; nc--; oc--; i--; } }
  if (nc == 0) return 0; double r = n[0];
  for (int i = 0; i < oc; i++) { if (o[i]=='+') r += n[i+1]; else if (o[i]=='-') r -= n[i+1]; }
  return r;
}

void pongReset() { pongPaddleLeftY=24; pongPaddleRightY=24; pongBallX=64; pongBallY=32; pongBallDX=1; pongBallDY=1; pongScore=0; pongGameOver=false; pongPauseItem=0; }
void pongInit() { pongReset(); pongLastUpdate=millis(); }
void pongUpdate() {
  if (pongGameOver) return;
  unsigned long n = millis(); int iv = 25 - pongSpeed * 5; if (iv < 8) iv = 8;
  if (n - pongLastUpdate < (unsigned long)iv) return;
  pongLastUpdate = n;
  pongBallX += pongBallDX; pongBallY += pongBallDY;
  if (pongBallY <= 2 || pongBallY >= 61) { pongBallDY = -pongBallDY; if (pongBallY <= 2) pongBallY = 2; if (pongBallY >= 61) pongBallY = 61; }
  if (pongBallX <= 4 && pongBallDX < 0) {
    if (pongBallY >= pongPaddleLeftY - 2 && pongBallY <= pongPaddleLeftY + 14) { pongBallDX = -pongBallDX; int hp = pongBallY - pongPaddleLeftY; if (hp < 4) pongBallDY = -1; else if (hp > 10) pongBallDY = 1; pongScore++; }
    else if (pongBallX <= 0) pongGameOver = true;
  }
  if (pongBallX >= 122 && pongBallDX > 0) {
    if (pongBallY >= pongPaddleRightY - 2 && pongBallY <= pongPaddleRightY + 14) { pongBallDX = -pongBallDX; int hp = pongBallY - pongPaddleRightY; if (hp < 4) pongBallDY = -1; else if (hp > 10) pongBallDY = 1; }
    else if (pongBallX >= 127) pongGameOver = true;
  }
  if (pongBallDX > 0) {
    int t = pongBallY - 6, sp = pongAILevel + 1;
    if (pongPaddleRightY < t) { pongPaddleRightY += sp; if (pongPaddleRightY > t) pongPaddleRightY = t; }
    else if (pongPaddleRightY > t) { pongPaddleRightY -= sp; if (pongPaddleRightY < t) pongPaddleRightY = t; }
  }
  if (pongPaddleRightY < 2) pongPaddleRightY = 2; if (pongPaddleRightY > 48) pongPaddleRightY = 48;
}
void drawPong() {
  u8g2.clearBuffer();
  for (int y = 2; y < 64; y += 4) { u8g2.drawPixel(63, y); u8g2.drawPixel(64, y); }
  u8g2.drawBox(0, pongPaddleLeftY, 2, 14); u8g2.drawBox(126, pongPaddleRightY, 2, 14);
  u8g2.drawBox(pongBallX, pongBallY, 3, 3);
  u8g2.setFont(FONT_TINY); u8g2.setCursor(2, 2); u8g2.print("S:"); u8g2.print(pongScore);
  if (pongGameOver) { u8g2.setFont(FONT_BIG); u8g2.setCursor(20, 32); u8g2.print("OVER"); }
  u8g2.sendBuffer();
}
void drawPongPause() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_BIG); u8g2.setCursor(40, 8); u8g2.print("PAUSE");
  const char* its[] = {"Cont", "Set", "Exit"};
  for (int i = 0; i < 3; i++) { int y = 28 + i * 12; u8g2.setCursor(0, y); u8g2.print(i == pongPauseItem ? "> " : "  "); u8g2.print(its[i]); }
  u8g2.sendBuffer();
}
void drawPongSettings() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0, 8); u8g2.print("Pong");
  u8g2.setCursor(0, 22); u8g2.print("Spd:"); u8g2.print(pongSpeed);
  u8g2.setCursor(0, 34); u8g2.print("AI:"); u8g2.print(pongAILevel);
  u8g2.setCursor(0, 52); u8g2.print("Nxt:chg");
  u8g2.sendBuffer();
}

void dodgerReset() {
  dodgerX = 60; dodgerY = 56;
  for (int i = 0; i < DODGER_MAX_OBSTACLES; i++) { dodgerObstacles[i][0] = random(2, 30) * 4; dodgerObstacles[i][1] = -random(4, 16) * 4; }
  dodgerScore = 0; dodgerGameOver = false; dodgerLastUpdate = millis(); dodgerUpdateInterval = 100; dodgerObstacleSpeed = 1; dodgerPauseItem = 0;
}
void dodgerUpdate() {
  if (dodgerGameOver) return;
  if (millis() - dodgerLastUpdate < (unsigned long)dodgerUpdateInterval) return;
  dodgerLastUpdate = millis();
  for (int i = 0; i < DODGER_MAX_OBSTACLES; i++) {
    dodgerObstacles[i][1] += dodgerObstacleSpeed;
    if (dodgerObstacles[i][1] > 64) { dodgerObstacles[i][0] = random(2, 30) * 4; dodgerObstacles[i][1] = -random(4, 12) * 4; dodgerScore++; }
    if (abs(dodgerObstacles[i][0] - dodgerX) < 8 && abs(dodgerObstacles[i][1] - dodgerY) < 6) dodgerGameOver = true;
  }
}
void drawDodger() {
  u8g2.clearBuffer(); u8g2.drawBox(dodgerX, dodgerY, 8, 6);
  for (int i = 0; i < DODGER_MAX_OBSTACLES; i++) if (dodgerObstacles[i][1] >= 0 && dodgerObstacles[i][1] < 64) u8g2.drawBox(dodgerObstacles[i][0], dodgerObstacles[i][1], 6, 6);
  u8g2.setFont(FONT_TINY); u8g2.setCursor(0, 8); u8g2.print("S:"); u8g2.print(dodgerScore);
  if (dodgerGameOver) { u8g2.setFont(FONT_BIG); u8g2.setCursor(20, 30); u8g2.print("OVER"); }
  u8g2.sendBuffer();
}
void drawDodgerPause() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_BIG); u8g2.setCursor(40, 8); u8g2.print("PAUSE");
  const char* its[] = {"Cont", "Exit"};
  for (int i = 0; i < 2; i++) { int y = 30 + i * 14; u8g2.setCursor(0, y); u8g2.print(i == dodgerPauseItem ? "> " : "  "); u8g2.print(its[i]); }
  u8g2.sendBuffer();
}

void dinoReset() {
  dinoX = 20; dinoY = DINO_GROUND_Y - DINO_H;
  dinoVelY = 0; dinoJumping = false; dinoOnGround = true;
  dinoScore = 0; dinoGameOver = false;
  dinoObsCount = 0;
  dinoLastUpdate = millis(); dinoRunTimer = millis();
  dinoSpeedUp = millis(); dinoCurrentSpeed = 4;
  dinoPauseItem = 0;
}
void dinoUpdate() {
  if (dinoGameOver) return;
  if (millis() - dinoLastUpdate < (unsigned long)DINO_UPDATE_MS) return;
  dinoLastUpdate = millis();
  if (millis() - dinoRunTimer > 100) { dinoRunFrame = (dinoRunFrame + 1) % 2; dinoRunTimer = millis(); }
  dinoVelY += 1;
  dinoY += dinoVelY;
  if (dinoY >= DINO_GROUND_Y - DINO_H) { dinoY = DINO_GROUND_Y - DINO_H; dinoVelY = 0; dinoJumping = false; dinoOnGround = true; }
  else { dinoOnGround = false; }
  for (int i = 0; i < dinoObsCount; i++) dinoObsX[i] -= dinoCurrentSpeed;
  while (dinoObsCount > 0 && dinoObsX[0] < -15) {
    for (int i = 0; i < dinoObsCount - 1; i++) { dinoObsX[i] = dinoObsX[i + 1]; dinoObsType[i] = dinoObsType[i + 1]; }
    dinoObsCount--;
  }
  static unsigned long lastSpawn = 0;
  if (dinoObsCount < MAX_DINO_OBST && millis() - lastSpawn > random(1500, 3500)) {
    dinoObsX[dinoObsCount] = SCREEN_W;
    int r = random(0, 100);
    dinoObsType[dinoObsCount] = (r < 60) ? 0 : (r < 90) ? 1 : 2;
    dinoObsCount++;
    lastSpawn = millis();
  }
  if (millis() - dinoSpeedUp > 10000) { if (dinoCurrentSpeed < 10) dinoCurrentSpeed++; dinoSpeedUp = millis(); }
  bool ducking = sendIsHeld || nextIsHeld;
  int dinoH = ducking ? DUCK_H : DINO_H;
  int dinoW = ducking ? DUCK_W : DINO_W;
  for (int i = 0; i < dinoObsCount; i++) {
    int ow, oh;
    if (dinoObsType[i] == 0) { ow = CACTUS_S_W; oh = CACTUS_S_H; }
    else if (dinoObsType[i] == 1) { ow = CACTUS_B_W; oh = CACTUS_B_H; }
    else { ow = BIRD_W; oh = BIRD_H; }
    int ox = dinoObsX[i];
    int oy = (dinoObsType[i] == 2) ? (DINO_GROUND_Y - BIRD_H - 5) : (DINO_GROUND_Y - oh);
    if (dinoX + dinoW - 2 > ox && dinoX + 2 < ox + ow && dinoY + dinoH - 2 > oy && dinoY + 2 < oy + oh) {
      if (dinoObsType[i] == 2 && ducking) continue;
      dinoGameOver = true;
      if (dinoScore > dinoHighScore) dinoHighScore = dinoScore;
      break;
    }
  }
  dinoScore = millis() / 100;
}
void drawBitmap(int x, int y, const uint8_t* sprite, int w, int h) {
  for (int row = 0; row < h; row++) for (int col = 0; col < w; col++) if (sprite[row * w + col]) u8g2.drawPixel(x + col, y + row);
}
void drawDino() {
  u8g2.clearBuffer();
  u8g2.drawHLine(0, DINO_GROUND_Y, SCREEN_W);
  u8g2.drawCircle(110, 12, 4);
  u8g2.drawBox(80, 8, 8, 2); u8g2.drawBox(82, 6, 4, 2);
  bool ducking = sendIsHeld || nextIsHeld;
  if (ducking) drawBitmap(dinoX, dinoY, (const uint8_t*)DINO_DUCK, DUCK_W, DUCK_H);
  else if (dinoRunFrame == 0) drawBitmap(dinoX, dinoY, (const uint8_t*)DINO_RUN1, DINO_W, DINO_H);
  else drawBitmap(dinoX, dinoY, (const uint8_t*)DINO_RUN2, DINO_W, DINO_H);
  for (int i = 0; i < dinoObsCount; i++) {
    int ox = dinoObsX[i];
    if (dinoObsType[i] == 0) drawBitmap(ox, DINO_GROUND_Y - CACTUS_S_H, (const uint8_t*)CACTUS_SMALL, CACTUS_S_W, CACTUS_S_H);
    else if (dinoObsType[i] == 1) drawBitmap(ox, DINO_GROUND_Y - CACTUS_B_H, (const uint8_t*)CACTUS_BIG, CACTUS_B_W, CACTUS_B_H);
    else drawBitmap(ox, DINO_GROUND_Y - BIRD_H - 5, (const uint8_t*)BIRD, BIRD_W, BIRD_H);
  }
  u8g2.setFont(FONT_TINY);
  u8g2.setCursor(2, 2); u8g2.print("HI "); u8g2.print(dinoHighScore);
  u8g2.setCursor(70, 2); u8g2.print(dinoScore);
  if (dinoCurrentSpeed > 5) { u8g2.setCursor(105, 2); u8g2.print("!"); }
  if (dinoGameOver) {
    u8g2.setFont(FONT_BIG);
    u8g2.setCursor(15, 25);
    u8g2.print("GAME OVER");
    u8g2.setFont(FONT_TINY);
    u8g2.setCursor(30, 45);
    u8g2.print("Nxt: restart");
  }
  u8g2.sendBuffer();
}
void drawDinoPause() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_BIG); u8g2.setCursor(40, 8); u8g2.print("PAUSE");
  const char* its[] = {"Cont", "Exit"};
  for (int i = 0; i < 2; i++) { int y = 30 + i * 14; u8g2.setCursor(0, y); u8g2.print(i == dinoPauseItem ? "> " : "  "); u8g2.print(its[i]); }
  u8g2.sendBuffer();
}

void drawSplash() {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_HUGE);
  int w1 = u8g2.getStrWidth("ESP8266");
  u8g2.setCursor((SCREEN_W - w1) / 2, 18);
  u8g2.print("ESP8266");
  u8g2.setFont(FONT_BIG);
  int w2 = u8g2.getStrWidth(FW_VERSION);
  u8g2.setCursor((SCREEN_W - w2) / 2, 35);
  u8g2.print(FW_VERSION);
  u8g2.setFont(FONT_TINY);
  u8g2.print("AI Terminal");
  u8g2.setCursor(60, 48);
  unsigned long elapsed = millis() - splashStartTime;
  int progress = min(100, (int)((elapsed * 100) / SPLASH_DURATION));
  u8g2.drawFrame(14, 54, 100, 8);
  u8g2.drawBox(16, 56, (96 * progress) / 100, 4);
  u8g2.sendBuffer();
}

void drawMainMenu() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_MENU);
  u8g2.setCursor(0,10); u8g2.print("Menu ["); u8g2.print(menuItem+1); u8g2.print("/"); u8g2.print(menuCount); u8g2.print("]");
  u8g2.drawHLine(0,12,SCREEN_W);
  int sh=5,st=menuItem-2; if(st<0)st=0; if(st>menuCount-sh)st=max(0,menuCount-sh);
  for(int i=0;i<sh;i++){int idx=st+i;if(idx>=menuCount)break; int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(idx==menuItem?"> ":"  "); u8g2.print(menuOrder[idx]);}
  drawNotifications(); u8g2.sendBuffer();
}
void drawStats() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_MENU);
  u8g2.setCursor(0,8); u8g2.print("Stats"); u8g2.drawHLine(0,10,SCREEN_W);
  u8g2.setCursor(0,20); u8g2.print("Wi:"); u8g2.print(wifiState==WIFI_CONNECTED?"OK":"OFF");
  if(wifiState==WIFI_CONNECTED){u8g2.setCursor(0,30);u8g2.print("R:");u8g2.print(WiFi.RSSI());u8g2.setCursor(0,40);u8g2.print("IP:");u8g2.print(WiFi.localIP());}
  u8g2.setCursor(0,50); u8g2.print("Ph:"); u8g2.print(PHONE_RELAY_HOST);
  u8g2.setCursor(0,60); u8g2.print("Sy:"); if(lastSyncTime==0)u8g2.print("..."); else{u8g2.print(lastSyncSuccess?"OK ":"F ");unsigned long a=(millis()-lastSyncTime)/1000;u8g2.print(a);}
  u8g2.sendBuffer();
}
void drawAIList() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_MENU);
  u8g2.setCursor(0,10); u8g2.print("AI [");u8g2.print(currentItem+1);u8g2.print("/");u8g2.print(promptCount);u8g2.print("]");
  u8g2.drawHLine(0,12,SCREEN_W);
  for(int i=0;i<min(promptCount,5);i++){int idx=(currentItem-2+i+promptCount)%promptCount; int y=22+i*10; u8g2.setCursor(0,y); u8g2.print(idx==currentItem?"> ":"  "); u8g2.print(prompts[idx]);}
  drawNotifications(); u8g2.sendBuffer();
}
void drawThinking() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Thinking..."); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,30); u8g2.print(prompts[currentItem]); u8g2.sendBuffer(); }
void drawResponse() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); for(int i=0;i<5;i++){int idx=responsePage*5+i;if(idx>=wrappedLineCount)break; u8g2.setCursor(0,11+i*10); u8g2.print(wrappedLines[idx]);} u8g2.setCursor(0,SCREEN_H-2);u8g2.print("[");u8g2.print(responsePage+1);u8g2.print("/");u8g2.print(totalResponsePages);u8g2.print("]"); u8g2.sendBuffer(); }
void drawTime() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Time"); u8g2.drawHLine(0,12,SCREEN_W); time_t n=time(nullptr); struct tm* v=localtime(&n); u8g2.setCursor(0,28); u8g2.print(formatTime(v)); u8g2.print(timeSynced?"*":"?"); u8g2.sendBuffer(); }
void drawNotice() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Note"); u8g2.drawHLine(0,12,SCREEN_W); for(int i=0;i<5;i++){int idx=noticePage*5+i;if(idx>=wrappedLineCount)break; u8g2.setCursor(0,21+i*10); u8g2.print(wrappedLines[idx]);} u8g2.sendBuffer(); }
void drawMapList() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Maps [");u8g2.print(currentMap+1);u8g2.print("/");u8g2.print(mapCount);u8g2.print("]"); u8g2.drawHLine(0,12,SCREEN_W); if(mapCount==0){u8g2.setCursor(0,28);u8g2.print("No maps");}else for(int i=0;i<5;i++){int idx=(currentMap-2+i+mapCount)%mapCount; int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(idx==currentMap?"> ":"  "); u8g2.print(mapNames[idx]);} u8g2.sendBuffer(); }
const char* SETTINGS_ITEMS[] = {"WiFi", "Order", "Help", "Bright", "Notif", "Sleep", "Sync"};
void drawSettings() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_MENU);
  u8g2.setCursor(0,10); u8g2.print("Set [");u8g2.print(settingsItem+1);u8g2.print("/7]");
  u8g2.drawHLine(0,12,SCREEN_W);
  int sh=5, st=settingsItem-2; if(st<0)st=0; if(st>7-sh)st=max(0,7-sh);
  for(int i=0;i<sh;i++){int idx=st+i;if(idx>=7)break; int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(idx==settingsItem?"> ":"  "); u8g2.print(SETTINGS_ITEMS[idx]);
    if(idx==3){u8g2.setCursor(100,y);u8g2.print(brightnessLevel);u8g2.print("%");}
    else if(idx==4){u8g2.setCursor(100,y);u8g2.print(notificationsEnabled?"ON":"OFF");}
    else if(idx==5){u8g2.setCursor(100,y);u8g2.print(sleepEnabled?"ON":"OFF");}
    else if(idx==6){u8g2.setCursor(100,y);u8g2.print(syncEnabled?"ON":"OFF");}
  }
  drawNotifications(); u8g2.sendBuffer();
}
void scanWifi() { wifiNetworkCount=0; int n=WiFi.scanNetworks(); for(int i=0;i<n&&wifiNetworkCount<MAX_WIFI_NETWORKS;i++){String s=WiFi.SSID(i); if(s.length()>0){bool ex=false;for(int j=0;j<wifiNetworkCount;j++)if(wifiNetworks[j]==s){ex=true;break;} if(!ex){wifiNetworks[wifiNetworkCount]=s; wifiNetworkOpen[wifiNetworkCount]=(WiFi.encryptionType(i)==ENC_TYPE_NONE); wifiNetworkCount++;}}} }
void drawWifiList() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("WiFi [");u8g2.print(wifiNetworkCount);u8g2.print("]"); u8g2.drawHLine(0,12,SCREEN_W); for(int i=0;i<min(wifiNetworkCount,5);i++){int idx=(wifiSelectedNet-2+i+wifiNetworkCount)%wifiNetworkCount; int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(idx==wifiSelectedNet?"> ":"  "); u8g2.print(wifiNetworks[idx]); if(wifiNetworkOpen[idx])u8g2.print("*");} u8g2.sendBuffer(); }
void drawKeyboard() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Pass:");u8g2.print(wifiPassword);u8g2.print("_"); u8g2.drawHLine(0,22,SCREEN_W); u8g2.setFont(FONT_BIG); for(int i=0;i<=9;i++){int c=i%3,r=i/3,x=20+c*35,y=32+r*11; if(i==keyboardDigit){u8g2.drawBox(x-2,y-9,18,11); u8g2.setDrawColor(0); u8g2.setCursor(x,y); u8g2.print(i); u8g2.setDrawColor(1);} else { u8g2.setCursor(x,y); u8g2.print(i); }} u8g2.setFont(FONT_TINY); u8g2.setCursor(0,SCREEN_H-2); u8g2.print("HldNxt:OK"); u8g2.sendBuffer(); }
void drawWifiConn() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,20); u8g2.print("WiFi..."); u8g2.setCursor(0,32); u8g2.print(currentSSID); u8g2.setCursor(0,50); int d=(millis()/500)%4; for(int i=0;i<d;i++)u8g2.print("."); u8g2.sendBuffer(); }
void drawMenuOrder() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Order"); u8g2.drawHLine(0,12,SCREEN_W); for(int i=0;i<min(menuCount,5);i++){int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(i==menuItem?"> ":"  "); u8g2.print(menuOrder[i]);} u8g2.sendBuffer(); }
void drawHelp() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Help"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,22); u8g2.print("Nxt:scrl HldNxt:act"); u8g2.setCursor(0,32); u8g2.print("Snd:sel HldBoth:slp"); u8g2.setCursor(0,48); u8g2.print("Dino: tapS=jump"); u8g2.setCursor(0,58); u8g2.print("tapN=duck HldS=pause"); u8g2.sendBuffer(); }
void drawBright() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Bright"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,28); u8g2.print("Lvl:");u8g2.print(brightnessLevel);u8g2.print("%"); u8g2.drawFrame(10,42,108,14); int bw=(brightnessLevel*104)/100; u8g2.drawBox(12,44,bw,10); u8g2.sendBuffer(); }
void drawNotif() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Notif"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(20,32); u8g2.print(notificationsEnabled?"[ON":"[OFF]"); u8g2.sendBuffer(); }
void drawSleep() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Sleep"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(20,32); u8g2.print(sleepEnabled?"[ON":"[OFF]"); u8g2.sendBuffer(); }
void drawSyncSet() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,8); u8g2.print("Sync"); u8g2.drawHLine(0,10,SCREEN_W); u8g2.setCursor(0,22); u8g2.print("ON:");u8g2.print(syncEnabled?"Y":"N"); u8g2.setCursor(0,32); u8g2.print("Ph:");u8g2.print(PHONE_RELAY_HOST); u8g2.setCursor(0,46); u8g2.print("HldNxt:stat"); u8g2.setCursor(0,56); u8g2.print("HldSnd:sync"); u8g2.sendBuffer(); }
void drawSyncStat() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,8); u8g2.print("Sync St"); u8g2.drawHLine(0,10,SCREEN_W); u8g2.setCursor(0,20); u8g2.print("Last:"); if(lastSyncTime==0)u8g2.print("none"); else{unsigned long a=(millis()-lastSyncTime)/1000;if(a<60)u8g2.print(a); else u8g2.print(a/60);} u8g2.setCursor(0,30); u8g2.print("P:");u8g2.print(promptCount); u8g2.setCursor(0,40); u8g2.print("N:");u8g2.print(notesCount); u8g2.setCursor(0,50); u8g2.print("M:");u8g2.print(messagesCount); u8g2.sendBuffer(); }
void drawSyncNow() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,20); u8g2.print("Sync..."); u8g2.sendBuffer(); }
const char* MSG_SUBMENU[] = {"Chat", "Tmpl", "Cust"}; int msgSubItem = 0;
void drawMsgMenu() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Msg [");u8g2.print(messagesCount);u8g2.print("]"); u8g2.drawHLine(0,12,SCREEN_W); for(int i=0;i<3;i++){int y=22+i*12; u8g2.setCursor(0,y); u8g2.print(i==msgSubItem?"> ":"  "); u8g2.print(MSG_SUBMENU[i]);} u8g2.sendBuffer(); }
void drawMsgChat() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Chat"); u8g2.drawHLine(0,12,SCREEN_W); if(messagesCount==0){u8g2.setCursor(0,30);u8g2.print("No msg");}else for(int i=0;i<min(messagesCount,5);i++){int idx=(currentMsg-2+i+messagesCount)%messagesCount; int y=22+i*10; u8g2.setCursor(0,y); u8g2.print(messagesFromESP[idx]?"[Y]":"[W]"); u8g2.print(" "); String t=messages[idx]; if(t.length()>15)t=t.substring(0,15)+".."; u8g2.print(t);} u8g2.sendBuffer(); }
void drawMsgChatView() { if(messagesCount==0){state=STATE_MSG_CHAT;return;} u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print(messagesFromESP[currentMsg]?"[You]":"[Web]"); u8g2.drawHLine(0,12,SCREEN_W); int l=wrapText(messages[currentMsg]); int tp=max(1,(l+4)/5); int sl=responsePage*5; for(int i=0;i<5;i++){int idx=sl+i;if(idx>=l)break; u8g2.setCursor(0,21+i*8); u8g2.print(wrappedLines[idx]);} u8g2.setCursor(0,SCREEN_H-2); u8g2.print("[");u8g2.print(responsePage+1);u8g2.print("/");u8g2.print(tp);u8g2.print("]"); u8g2.sendBuffer(); }
void drawMsgTemplates() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Tmpl"); u8g2.drawHLine(0,12,SCREEN_W); for(int i=0;i<min(templateCount,5);i++){int idx=(currentTemplate-2+i+templateCount)%templateCount; int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(idx==currentTemplate?"> ":"  "); String t=templates[idx]; if(t.length()>18)t=t.substring(0,18)+".."; u8g2.print(t);} u8g2.sendBuffer(); }
void drawMsgCustom() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Msg"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,22); u8g2.print(customMessage);u8g2.print("_"); u8g2.drawHLine(0,32,SCREEN_W); u8g2.setCursor(0,44); u8g2.print(">");u8g2.print(LETTER_GROUPS[keyboardGroup]);u8g2.print("<"); u8g2.sendBuffer(); }
void drawMsgCustomLetter() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Msg"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,22); u8g2.print(customMessage);u8g2.print("_"); u8g2.setFont(FONT_BIG); const char* g=LETTER_GROUPS[keyboardGroup]; for(int i=0;i<5&&g[i];i++){int x=10+i*22,y=50; if(i==keyboardLetter){u8g2.drawBox(x-2,y-10,18,12); u8g2.setDrawColor(0); u8g2.setCursor(x,y); u8g2.print(g[i]); u8g2.setDrawColor(1);} else { u8g2.setCursor(x,y); u8g2.print(g[i]); }} u8g2.sendBuffer(); }
void drawCalculator() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Calc:"); if(calcState==CALC_RESULT){u8g2.print(" = ");u8g2.print(formatCalc(calcResult));}else{u8g2.print(" ");u8g2.print(calcExpr);u8g2.print("_");} u8g2.drawHLine(0,20,SCREEN_W); u8g2.setCursor(0,32); if(calcState==CALC_DIGIT){u8g2.print("[");u8g2.print(CALC_DIGITS[calcDigitOrOp]);u8g2.print("]");}else if(calcState==CALC_OP){u8g2.print("[");u8g2.print(CALC_OPS[calcDigitOrOp]);u8g2.print("]");} else u8g2.print("[OK]"); u8g2.setCursor(0,44); if(calcState==CALC_RESULT)u8g2.print("Snd:cont"); else{u8g2.print("Nxt:cyc Snd:+");} u8g2.setFont(FONT_TINY); u8g2.setCursor(0,SCREEN_H-2); u8g2.print("HldNxt:= HldSnd:ext"); u8g2.sendBuffer(); }
void drawNotesMenu() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Notes [");u8g2.print(notesCount);u8g2.print("]"); u8g2.drawHLine(0,12,SCREEN_W); for(int i=0;i<2;i++){int y=24+i*16; u8g2.setCursor(0,y); u8g2.print(i==notesMenuItem?"> ":"  "); u8g2.print(i==0?"Add":"Hist");} u8g2.sendBuffer(); }
void drawNotesAdd() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Note"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,22); u8g2.print(noteText);u8g2.print("_"); u8g2.setCursor(0,44); u8g2.print(">");if(noteKeyboardGroup<5)u8g2.print(LETTER_GROUPS[noteKeyboardGroup]); else if(noteKeyboardGroup==5)u8g2.print("09"); else u8g2.print("[]"); u8g2.print("<"); u8g2.sendBuffer(); }
void drawNotesAddLetter() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Note"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,22); u8g2.print(noteText);u8g2.print("_"); u8g2.setFont(FONT_BIG); if(noteKeyboardGroup<5){const char* g=LETTER_GROUPS[noteKeyboardGroup]; for(int i=0;i<5&&g[i];i++){int x=10+i*22,y=56; if(i==noteKeyboardLetter){u8g2.drawBox(x-2,y-10,18,12); u8g2.setDrawColor(0); u8g2.setCursor(x,y); u8g2.print(g[i]); u8g2.setDrawColor(1);} else { u8g2.setCursor(x,y); u8g2.print(g[i]); }}} else if(noteKeyboardGroup==5){for(int i=0;i<5;i++){int x=10+i*22,y=56; if(i==noteKeyboardLetter){u8g2.drawBox(x-2,y-10,18,12); u8g2.setDrawColor(0); u8g2.setCursor(x,y); u8g2.print(NOTE_DIGITS_GROUP[i]); u8g2.setDrawColor(1);} else { u8g2.setCursor(x,y); u8g2.print(NOTE_DIGITS_GROUP[i]); }}} else { int x=50,y=56; u8g2.drawBox(x-2,y-10,32,12); u8g2.setDrawColor(0); u8g2.setCursor(x+4,y); u8g2.print("SPC"); u8g2.setDrawColor(1); } u8g2.sendBuffer(); }
void drawNotesHist() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Notes"); u8g2.drawHLine(0,12,SCREEN_W); if(notesCount==0){u8g2.setCursor(0,30);u8g2.print("Empty");}else for(int i=0;i<min(notesCount,5);i++){int idx=(currentNote-2+i+notesCount)%notesCount; int y=22+i*8; u8g2.setCursor(0,y); u8g2.print(idx==currentNote?"> ":"  "); u8g2.print(idx+1);u8g2.print(". "); String t=notes[idx]; if(t.length()>14)t=t.substring(0,14)+".."; u8g2.print(t);} u8g2.sendBuffer(); }
void drawNotesView() { if(notesCount==0){state=STATE_NOTES_HISTORY;return;} u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Note#");u8g2.print(currentNote+1); u8g2.drawHLine(0,12,SCREEN_W); int l=wrapText(notes[currentNote]); int tp=max(1,(l+4)/5); int sl=responsePage*5; for(int i=0;i<5;i++){int idx=sl+i;if(idx>=l)break; u8g2.setCursor(0,21+i*8); u8g2.print(wrappedLines[idx]);} u8g2.setCursor(0,SCREEN_H-2); u8g2.print("[");u8g2.print(responsePage+1);u8g2.print("/");u8g2.print(tp);u8g2.print("]"); u8g2.sendBuffer(); }
const char* GAMES_SUBMENU[] = {"Pong", "Dodg", "Dino", "Scor"};
void drawGamesMenu() {
  u8g2.clearBuffer(); u8g2.setFont(FONT_MENU);
  u8g2.setCursor(0,10); u8g2.print("Games");
  u8g2.drawHLine(0,12,SCREEN_W);
  for(int i=0;i<4;i++){int y=22+i*10; u8g2.setCursor(0,y); u8g2.print(i==gamesMenuItem?"> ":"  "); u8g2.print(GAMES_SUBMENU[i]);}
  u8g2.sendBuffer();
}
void drawGamesScore() { u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0,10); u8g2.print("Scores"); u8g2.drawHLine(0,12,SCREEN_W); u8g2.setCursor(0,24); u8g2.print("Pong:");u8g2.print(pongScore); u8g2.setCursor(0,34); u8g2.print("Dodg:");u8g2.print(dodgerScore); u8g2.setCursor(0,44); u8g2.print("Dino HI:");u8g2.print(dinoHighScore); u8g2.sendBuffer(); }

const char index_html[] PROGMEM = R"INDEX(
<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP v8.6</title><style>*{box-sizing:border-box}body{font-family:sans-serif;background:#1a1a1a;color:#e0e0e0;padding:15px;margin:0}.c{background:#2a2a2a;border-radius:8px;padding:15px;margin-bottom:15px}h1{color:#4fc3f7;font-size:20px}h2{color:#81d4fa;font-size:16px;border-bottom:1px solid #333;padding-bottom:5px}input,textarea{background:#1a1a1a;border:1px solid #444;color:#fff;padding:6px;border-radius:4px;width:100%;margin:5px 0;box-sizing:border-box}button{background:#4fc3f7;color:#000;border:none;padding:6px 12px;border-radius:4px;cursor:pointer;font-weight:bold;margin:3px}button:hover{background:#29b6f6}button.d{background:#ef5350;color:#fff}.p{display:flex;gap:5px;margin:5px 0;align-items:center;flex-wrap:wrap}.p input{flex:1;min-width:200px}.l{background:#000;padding:8px;border-radius:4px;font-family:monospace;font-size:11px;max-height:200px;overflow-y:auto}.i{color:#4fc3f7;font-size:13px;line-height:1.6}.ok{color:#4caf50}.er{color:#f44336}</style></head><body><h1>ESP v8.6</h1>
<div class=c><h2>Status</h2><div class=i id=s>...</div></div>
<div class=c><h2>Prompts</h2><div id=p>...</div><div class=p><input id=np maxlength=60 placeholder='New prompt'><button onclick=addPr()>Add</button></div></div>
<div class=c><h2>Notes</h2><div id=n>...</div><textarea id=nn rows=2 maxlength=200></textarea><div class=p><button onclick=addN()>Add</button><button class=d onclick=clrN()>Clear</button></div></div>
<div class=c><h2>Messages</h2><div id=m style=max-height:200px;overflow-y:auto>...</div><div class=p><input id=mi maxlength=180 placeholder='Message'><button onclick=addM()>Send</button></div></div>
<div class=c><h2>Notice</h2><textarea id=nt rows=2 maxlength=500></textarea><div class=p><button onclick=setN()>Set</button></div></div>
<div class=c><h2>Logs</h2><div class=l id=l>...</div></div>
<script>
async function u(){try{const s=await(await fetch('/s')).json();document.getElementById('s').innerHTML='Wi:'+s.w+'<br>R:'+s.r+'dBm<br>IP:'+s.ip+'<br>Phone:'+s.ph+'<br>Sy:'+(s.ok?'<span class=ok>OK</span>':'<span class=er>FAIL</span>');const ps=await(await fetch('/p')).json();let h='';ps.ps.forEach((p,i)=>{h+=`<div class=p><span>${i+1}.</span><input id=p${i} value="${p.replace(/"/g,'&quot;')}"><button onclick=upP(${i})>Save</button><button class=d onclick=dlP(${i})>Del</button></div>`;});document.getElementById('p').innerHTML=h;const ns=await(await fetch('/n')).json();let nh='';ns.ns.forEach((n,i)=>{nh+=`<div class=p><span>${i+1}.</span><input id=n${i} value="${n.replace(/"/g,'&quot;')}"><button onclick=upN(${i})>Save</button><button class=d onclick=dlN(${i})>Del</button></div>`;});document.getElementById('n').innerHTML=nh;const ms=await(await fetch('/m')).json();let mh='';ms.ms.forEach(m=>{mh+=`<div style="padding:5px;background:#333;margin:3px;border-radius:4px">[${m.f?'Y':'W'}] ${m.t}</div>`;});document.getElementById('m').innerHTML=mh||'No messages';document.getElementById('nt').value=(await(await fetch('/nt')).json()).t;}catch(e){}}
async function lg(){const r=await(await fetch('/l')).json();let h='';r.l.forEach(x=>{h+=`<div>${x}</div>`;});document.getElementById('l').innerHTML=h;}
async function addPr(){const t=document.getElementById('np').value.trim();if(!t)return;await fetch('/p?a='+encodeURIComponent(t));u();}
async function upP(i){await fetch('/p?u='+i+'&t='+encodeURIComponent(document.getElementById('p'+i).value));}
async function dlP(i){await fetch('/p?d='+i);u();}
async function addN(){const t=document.getElementById('nn').value.trim();if(!t)return;await fetch('/n?a='+encodeURIComponent(t));u();}
async function upN(i){await fetch('/n?u='+i+'&t='+encodeURIComponent(document.getElementById('n'+i).value));}
async function dlN(i){await fetch('/n?d='+i);u();}
async function clrN(){if(confirm('Clear?'))await fetch('/n?c=1');u();}
async function addM(){const t=document.getElementById('mi').value.trim();if(!t)return;await fetch('/m?a='+encodeURIComponent(t));u();}
async function setN(){await fetch('/nt?t='+encodeURIComponent(document.getElementById('nt').value));u();}
u();lg();setInterval(()=>{u();lg();},3000);
</script></body></html>
)INDEX";
void sendCORS() { server.sendHeader("Access-Control-Allow-Origin", "*"); }
String htmlEsc(const String& s) { String r = s; r.replace("&", "&amp;"); r.replace("<", "&lt;"); r.replace(">", "&gt;"); r.replace("\"", "&quot;"); return r; }
void handleRoot() { sendCORS(); server.send_P(200, "text/html", index_html); }

// Статус устройства: IP, RSSI, адрес ретранслятора и счётчики данных
void handleS() { sendCORS(); String j = "{\"ip\":\"" + WiFi.localIP().toString() + "\",\"w\":\"" + (wifiState == WIFI_CONNECTED ? "OK" : "OFF") + "\",\"r\":" + String(WiFi.RSSI()) + ",\"ph\":\"" + PHONE_RELAY_HOST + "\",\"ok\":" + String(lastSyncSuccess ? "true" : "false") + ",\"p\":" + String(promptCount) + ",\"n\":" + String(notesCount) + ",\"m\":" + String(messagesCount) + "}"; server.send(200, "application/json", j); }

// /p — список промптов, либо действие (a=add, d=del, u+t=update)
void handleP() {
  sendCORS();
  if (server.hasArg("a")) addPrompt(urlDecode(server.arg("a")));
  else if (server.hasArg("d")) delPrompt(server.arg("d").toInt());
  else if (server.hasArg("u") && server.hasArg("t")) updPrompt(server.arg("u").toInt(), urlDecode(server.arg("t")));
  String j = "{\"ps\":[";
  for (int i = 0; i < promptCount; i++) { if (i) j += ","; j += "\"" + htmlEsc(prompts[i]) + "\""; }
  j += "]}"; server.send(200, "application/json", j);
}

// /n — список заметок, либо действие (a=add, c=clear, d=del, u+t=update)
void handleN() {
  sendCORS();
  if (server.hasArg("a")) addNote(urlDecode(server.arg("a")));
  else if (server.hasArg("c")) clearNotes();
  else if (server.hasArg("d")) delNote(server.arg("d").toInt());
  else if (server.hasArg("u") && server.hasArg("t")) { int i = server.arg("u").toInt(); if (i >= 0 && i < notesCount) { notes[i] = urlDecode(server.arg("t")); saveNotes(); } }
  String j = "{\"ns\":[";
  for (int i = 0; i < notesCount; i++) { if (i) j += ","; j += "\"" + htmlEsc(notes[i]) + "\""; }
  j += "]}"; server.send(200, "application/json", j);
}

// /m — список сообщений, либо действие (a=add)
void handleM() {
  sendCORS();
  if (server.hasArg("a")) addMessage(urlDecode(server.arg("a")), false);
  String j = "{\"ms\":[";
  for (int i = 0; i < messagesCount; i++) { if (i) j += ","; j += "{\"t\":\"" + htmlEsc(messages[i]) + "\",\"f\":" + String(messagesFromESP[i] ? "true" : "false") + "}"; }
  j += "]}"; server.send(200, "application/json", j);
}
void handleNT() { sendCORS(); if (server.hasArg("t")) { String t = urlDecode(server.arg("t")); noticeText = t.length() > 0 ? t : "No notice"; } server.send(200, "application/json", "{\"t\":\"" + htmlEsc(noticeText) + "\"}"); }
void handleL() { sendCORS(); String j = "{\"l\":["; int st = (logIndex - logCount + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE; for (int i = 0; i < logCount; i++) { if (i) j += ","; j += "\"" + htmlEsc(logBuffer[(st + i) % LOG_BUFFER_SIZE]) + "\""; } j += "]}"; server.send(200, "application/json", j); }
void handle404() { sendCORS(); server.send(404, "text/plain", "404"); }

void setup() {
  Serial.begin(115200); delay(100);
  addLog("\n=== BOOT " + String(FW_VERSION) + " ===");
  ESP.wdtEnable(WDTO_8S);
  pinMode(BTN_NEXT, INPUT_PULLUP); pinMode(BTN_SEND, INPUT_PULLUP);
  if (!SPIFFS.begin()) { addLog("[FS] FAIL"); }
  else {
    if (!SPIFFS.exists("/maps")) { File f = SPIFFS.open("/maps/.keep", "w"); if (f) f.close(); }
    loadSettings(); loadPrompts(); loadNotes(); loadMessages(); loadMapList();
  }
  initPrompts(); initMenuOrder(); initTemplates(); calcReset();
  wrappedLineCount = wrapText(noticeText);
  totalResponsePages = max(1, (wrappedLineCount + 4) / 5);
  u8g2.begin(); u8g2.setFont(FONT_MENU);
  splashStartTime = millis();
  startWifiAttempt(PHONE_RELAY_FALLBACK, "");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/s", HTTP_GET, handleS);
  server.on("/p", HTTP_GET, handleP);   // список и действия с промптами
  server.on("/n", HTTP_GET, handleN);   // список и действия с заметками
  server.on("/m", HTTP_GET, handleM);   // список и действия с сообщениями
  server.on("/nt", HTTP_GET, handleNT);
  server.on("/l", HTTP_GET, handleL);
  server.onNotFound(handle404);
  server.begin();
  addLog("[Web] Started");
}

void loop() {
  ESP.wdtFeed();
  server.handleClient();
  ESP.wdtFeed();
  updateNotifications();
  wifiLoop();
  ESP.wdtFeed();

  if (state == STATE_SPLASH) {
    if (millis() - splashStartTime > SPLASH_DURATION) state = STATE_MENU;
    else { drawSplash(); ESP.wdtFeed(); delay(50); return; }
  }

  if (wifiState == WIFI_CONNECTED && !syncInProgress && millis() - lastSyncCheck > SYNC_INTERVAL) {
    lastSyncCheck = millis();
    doSync();
  }
  if (wifiState == WIFI_CONNECTED && !timeSynced) syncTime();

  bool nextBtn = (digitalRead(BTN_NEXT) == LOW);
  bool sendBtn = (digitalRead(BTN_SEND) == LOW);
  nextIsHeld = nextBtn;
  sendIsHeld = sendBtn;

  // Удержание обеих кнопок вместе = вход/выход из режима сна.
  if (nextBtn && sendBtn && sleepEnabled && !inSleepMode) {
    static unsigned long bs = 0;
    if (bs == 0) bs = millis();
    if (millis() - bs >= HOLD_MS) {
      u8g2.clearBuffer(); u8g2.setFont(FONT_MENU); u8g2.setCursor(0, 10); u8g2.print("Sleep?");
      u8g2.drawHLine(0, 12, SCREEN_W); u8g2.setCursor(0, 30); u8g2.print("Nxt:Y Snd:N");
      u8g2.sendBuffer();
      while (digitalRead(BTN_NEXT) == LOW && digitalRead(BTN_SEND) == LOW) { delay(10); ESP.wdtFeed(); }
      bs = 0;
      if (digitalRead(BTN_NEXT) == LOW) { inSleepMode = true; u8g2.setPowerSave(1); WiFi.setSleep(true); }
    }
  } else if (inSleepMode && nextBtn && sendBtn) {
    static unsigned long ws = 0;
    if (ws == 0) ws = millis();
    if (millis() - ws >= HOLD_MS) { inSleepMode = false; ws = 0; u8g2.setPowerSave(0); WiFi.setSleep(false); }
  }
  if (inSleepMode) { delay(100); ESP.wdtFeed(); return; }

  static unsigned long nps = 0, sps = 0;
  static bool nwp = false, nhf = false, swp = false, shf = false;

  // ------------- Кнопка NEXT -------------
  // Тоже самое "удержание/тап": пока держим — наращиваем таймер, при
  // отпускании смотрим, был ли это короткий тап или долгий холд.
  if (nextBtn && !nwp) { nps = millis(); nwp = true; nhf = false; }
  if (nextBtn && nwp && !nhf && millis() - nps >= HOLD_MS) {
    nhf = true;
    if (state == STATE_MENU) state = STATE_STATS;
    else if (state == STATE_AI_LIST) {
      if (wifiState != WIFI_CONNECTED) addNotification("No WiFi");
      else {
        state = STATE_AI_THINKING; drawThinking();
        responseText = askAI(prompts[currentItem]);
        wrappedLineCount = wrapText(responseText);
        totalResponsePages = max(1, (wrappedLineCount + 4) / 5);
        responsePage = 0;
        state = STATE_AI_RESPONSE;
      }
    }
    else if (state == STATE_MAP_LIST && mapCount > 0) state = STATE_MAP_VIEW;
    else if (state == STATE_SETTINGS) {
      if (settingsItem == 0) { state = STATE_SETUP_WIFI_LIST; scanWifi(); }
      else if (settingsItem == 1) state = STATE_SETUP_ORDER;
      else if (settingsItem == 2) state = STATE_SETUP_HELP;
      else if (settingsItem == 3) state = STATE_SETUP_BRIGHTNESS;
      else if (settingsItem == 4) state = STATE_SETUP_NOTIFY;
      else if (settingsItem == 5) state = STATE_SETUP_SLEEP;
      else if (settingsItem == 6) state = STATE_SETUP_SYNC;
    }
    else if (state == STATE_SETUP_WIFI_LIST) { scanWifi(); wifiSelectedNet = 0; }
    else if (state == STATE_SETUP_WIFI_PASS) {
      if (wifiPassword.length() >= 8) { state = STATE_SETUP_WIFI_CONNECTING; startWifiAttempt(wifiNetworks[wifiSelectedNet], wifiPassword); }
      else addNotification("Min 8");
    }
    else if (state == STATE_SETUP_SYNC) state = STATE_SYNC_STATUS;
    else if (state == STATE_SYNC_STATUS) state = STATE_SETUP_SYNC;
    else if (state >= STATE_SETUP_ORDER && state <= STATE_SETUP_SYNC) { state = STATE_SETTINGS; saveSettings(); }
    else if (state == STATE_MSG_MENU) {
      if (msgSubItem == 0) { state = STATE_MSG_CHAT; currentMsg = messagesCount > 0 ? messagesCount - 1 : 0; }
      else if (msgSubItem == 1) { state = STATE_MSG_TEMPLATES; currentTemplate = 0; }
      else { state = STATE_MSG_CUSTOM; customMessage = ""; keyboardGroup = 0; }
    }
    else if (state == STATE_MSG_CHAT && messagesCount > 0) { state = STATE_MSG_CHAT_VIEW; responsePage = 0; }
    else if (state == STATE_MSG_CUSTOM && customMessage.length() > 0) { addMessage(customMessage, true); customMessage = ""; state = STATE_MSG_MENU; }
    else if (state == STATE_MSG_CUSTOM_LETTER) { customMessage += String(LETTER_GROUPS[keyboardGroup][keyboardLetter]); state = STATE_MSG_CUSTOM; keyboardLetter = 0; }
    else if (state == STATE_NOTES_MENU) {
      state = notesMenuItem == 0 ? STATE_NOTES_ADD : STATE_NOTES_HISTORY;
      if (state == STATE_NOTES_ADD) { noteText = ""; noteKeyboardGroup = 0; } else currentNote = 0;
    }
    else if (state == STATE_NOTES_HISTORY && notesCount > 0) { state = STATE_NOTES_VIEW; responsePage = 0; }
    else if (state == STATE_NOTES_ADD && noteText.length() > 0) { addNote(noteText); noteText = ""; state = STATE_NOTES_MENU; }
    else if (state == STATE_NOTES_ADD_LETTER) {
      if (noteKeyboardGroup < 5) noteText += String(LETTER_GROUPS[noteKeyboardGroup][noteKeyboardLetter]);
      else if (noteKeyboardGroup == 5) noteText += String(NOTE_DIGITS_GROUP[noteKeyboardLetter]);
      else noteText += " ";
      state = STATE_NOTES_ADD; noteKeyboardLetter = 0;
    }
    else if (state == STATE_CALCULATOR) calcCompute();
    else if (state == STATE_GAMES_MENU) {
      if (gamesMenuItem == 0) { pongInit(); state = STATE_GAME_PONG; }
      else if (gamesMenuItem == 1) { dodgerReset(); state = STATE_GAME_DODGER; }
      else if (gamesMenuItem == 2) { dinoReset(); state = STATE_GAME_DINO; }
      else if (gamesMenuItem == 3) state = STATE_GAMES_MENU;
    }
    else if (state == STATE_GAME_PONG && pongGameOver) pongReset();
    else if (state == STATE_GAME_DODGER && dodgerGameOver) dodgerReset();
    else if (state == STATE_GAME_DINO && dinoGameOver) dinoReset();
  }

  if (!nextBtn && nwp) {
    unsigned long d = millis() - nps;
    nwp = false;
    if (!nhf && d < HOLD_MS && millis() - lastBtnTime > DEBOUNCE_MS) {
      lastBtnTime = millis();
      if (state == STATE_MENU) menuItem = (menuItem + 1) % menuCount;
      else if (state == STATE_SETTINGS) settingsItem = (settingsItem + 1) % 7;
      else if (state == STATE_AI_LIST) currentItem = (currentItem + 1) % promptCount;
      else if (state == STATE_AI_RESPONSE) { responsePage++; if (responsePage >= totalResponsePages) responsePage = 0; }
      else if (state == STATE_NOTICE) { noticePage++; if (noticePage >= totalResponsePages) noticePage = 0; }
      else if (state == STATE_MAP_LIST) { if (mapCount > 0) currentMap = (currentMap + 1) % mapCount; }
      else if (state == STATE_SETUP_WIFI_LIST) { if (wifiNetworkCount > 0) wifiSelectedNet = (wifiSelectedNet + 1) % wifiNetworkCount; }
      else if (state == STATE_SETUP_WIFI_PASS) keyboardDigit = (keyboardDigit + 1) % 10;
      else if (state == STATE_SETUP_ORDER) { if (menuItem < menuCount - 1) { String t = menuOrder[menuItem]; menuOrder[menuItem] = menuOrder[menuItem + 1]; menuOrder[menuItem + 1] = t; menuItem++; } }
      else if (state == STATE_SETUP_BRIGHTNESS) { brightnessLevel = brightnessLevel >= 100 ? 25 : brightnessLevel + 25; }
      else if (state == STATE_SETUP_NOTIFY) notificationsEnabled = !notificationsEnabled;
      else if (state == STATE_SETUP_SLEEP) sleepEnabled = !sleepEnabled;
      else if (state == STATE_SETUP_SYNC) syncEnabled = !syncEnabled;
      else if (state == STATE_MSG_MENU) msgSubItem = (msgSubItem + 1) % 3;
      else if (state == STATE_MSG_CHAT) { if (messagesCount > 0) currentMsg = (currentMsg + 1) % messagesCount; }
      else if (state == STATE_MSG_CHAT_VIEW) { int l = wrapText(messages[currentMsg]); int tp = max(1, (l + 4) / 5); responsePage++; if (responsePage >= tp) responsePage = 0; }
      else if (state == STATE_MSG_TEMPLATES) { if (templateCount > 0) currentTemplate = (currentTemplate + 1) % templateCount; }
      else if (state == STATE_MSG_CUSTOM) keyboardGroup = (keyboardGroup + 1) % LETTER_GROUPS_COUNT;
      else if (state == STATE_MSG_CUSTOM_LETTER) { const char* g = LETTER_GROUPS[keyboardGroup]; keyboardLetter = (keyboardLetter + 1) % strlen(g); }
      else if (state == STATE_NOTES_MENU) notesMenuItem = (notesMenuItem + 1) % 2;
      else if (state == STATE_NOTES_HISTORY) { if (notesCount > 0) currentNote = (currentNote + 1) % notesCount; }
      else if (state == STATE_NOTES_VIEW) { int l = wrapText(notes[currentNote]); int tp = max(1, (l + 4) / 5); responsePage++; if (responsePage >= tp) responsePage = 0; }
      else if (state == STATE_NOTES_ADD) noteKeyboardGroup = (noteKeyboardGroup + 1) % 7;
      else if (state == STATE_NOTES_ADD_LETTER) noteKeyboardLetter = (noteKeyboardLetter + 1) % 5;
      else if (state == STATE_CALCULATOR) calcCycle();
      else if (state == STATE_GAMES_MENU) gamesMenuItem = (gamesMenuItem + 1) % 4;
      else if (state == STATE_GAME_PONG_PAUSE) pongPauseItem = (pongPauseItem + 1) % 3;
      else if (state == STATE_GAME_PONG_SETTINGS) { pongSpeed++; if (pongSpeed > 4) pongSpeed = 1; }
      else if (state == STATE_GAME_DODGER_PAUSE) dodgerPauseItem = (dodgerPauseItem + 1) % 2;
      else if (state == STATE_GAME_DINO_PAUSE) dinoPauseItem = (dinoPauseItem + 1) % 2;
    }
    nhf = false;
  }

  // ------------- Кнопка SEND -------------
  // Та же логика: короткий тап — выбор/действие, удержание — отдельная команда
  // (например, в меню — переход в настройки, в Dino — поставить на паузу).
  if (sendBtn && !swp) { sps = millis(); swp = true; shf = false; }
  if (sendBtn && swp && !shf) {
    if (millis() - sps >= HOLD_MS) {
      shf = true;
      if (state == STATE_GAME_DINO && !dinoGameOver) { state = STATE_GAME_DINO_PAUSE; dinoPauseItem = 0; }
      else if (state == STATE_GAME_PONG_PAUSE) state = STATE_GAME_PONG;
      else if (state == STATE_GAME_DODGER_PAUSE) state = STATE_GAME_DODGER;
      else if (state == STATE_GAME_DINO_PAUSE) {
        if (dinoPauseItem == 1) { dinoReset(); state = STATE_GAMES_MENU; gamesMenuItem = 2; }
        else state = STATE_GAME_DINO;
      }
      else if (state == STATE_MENU) { state = STATE_SETTINGS; settingsItem = 0; }
      else if (state == STATE_CALCULATOR) { calcReset(); state = STATE_MENU; }
      else if (state == STATE_MSG_CUSTOM_LETTER) state = STATE_MSG_CUSTOM;
      else if (state == STATE_NOTES_ADD_LETTER) state = STATE_NOTES_ADD;
      else if (state == STATE_MSG_CHAT_VIEW) state = STATE_MSG_CHAT;
      else if (state == STATE_NOTES_VIEW) state = STATE_NOTES_HISTORY;
      else if (state == STATE_SYNC_NOW) state = STATE_SYNC_STATUS;
      else state = STATE_MENU;
    }
  }

  if (!sendBtn && swp) {
    unsigned long d = millis() - sps;
    swp = false;
    if (!shf && d < HOLD_MS && millis() - lastBtnTime > DEBOUNCE_MS) {
      lastBtnTime = millis();
      if (state == STATE_GAME_PONG && !pongGameOver) { state = STATE_GAME_PONG_PAUSE; pongPauseItem = 0; }
      else if (state == STATE_GAME_DODGER && !dodgerGameOver) { state = STATE_GAME_DODGER_PAUSE; dodgerPauseItem = 0; }
      else if (state == STATE_GAME_DINO && !dinoGameOver) {
        if (dinoOnGround) { dinoVelY = -10; dinoJumping = true; dinoOnGround = false; }
      }
      else if (state == STATE_GAME_PONG_PAUSE) {
        if (pongPauseItem == 0) state = STATE_GAME_PONG;
        else if (pongPauseItem == 1) state = STATE_GAME_PONG_SETTINGS;
        else { pongReset(); state = STATE_GAMES_MENU; gamesMenuItem = 0; }
      }
      else if (state == STATE_GAME_DODGER_PAUSE) {
        if (dodgerPauseItem == 0) state = STATE_GAME_DODGER;
        else { dodgerReset(); state = STATE_GAMES_MENU; gamesMenuItem = 1; }
      }
      else if (state == STATE_GAME_DINO_PAUSE) {
        if (dinoPauseItem == 0) state = STATE_GAME_DINO;
        else { dinoReset(); state = STATE_GAMES_MENU; gamesMenuItem = 2; }
      }
      else if (state == STATE_GAME_PONG && pongGameOver) { pongReset(); state = STATE_GAME_PONG; }
      else if (state == STATE_GAME_DODGER && dodgerGameOver) { dodgerReset(); state = STATE_GAME_DODGER; }
      else if (state == STATE_GAME_DINO && dinoGameOver) { dinoReset(); state = STATE_GAME_DINO; }
      else if (state == STATE_MENU) {
        String& it = menuOrder[menuItem];
        if (it == "AI Chat") state = STATE_AI_LIST;
        else if (it == "Time") state = STATE_TIME;
        else if (it == "Notice") { wrappedLineCount = wrapText(noticeText); totalResponsePages = max(1, (wrappedLineCount + 4) / 5); noticePage = 0; state = STATE_NOTICE; }
        else if (it == "Maps") { state = STATE_MAP_LIST; currentMap = 0; }
        else if (it == "Calc") { state = STATE_CALCULATOR; calcReset(); }
        else if (it == "Msg") { state = STATE_MSG_MENU; msgSubItem = 0; }
        else if (it == "Notes") { state = STATE_NOTES_MENU; notesMenuItem = 0; }
        else if (it == "Games") { state = STATE_GAMES_MENU; gamesMenuItem = 0; }
      }
      else if (state == STATE_STATS) state = STATE_MENU;
      else if (state == STATE_AI_LIST) state = STATE_MENU;
      else if (state == STATE_AI_RESPONSE) state = STATE_AI_LIST;
      else if (state == STATE_TIME) state = STATE_MENU;
      else if (state == STATE_NOTICE) state = STATE_MENU;
      else if (state == STATE_MAP_LIST) state = STATE_MENU;
      else if (state == STATE_SETTINGS) { state = STATE_MENU; saveSettings(); }
      else if (state == STATE_SETUP_WIFI_LIST) {
        if (wifiNetworkCount > 0) {
          if (wifiNetworkOpen[wifiSelectedNet]) { state = STATE_SETUP_WIFI_CONNECTING; startWifiAttempt(wifiNetworks[wifiSelectedNet], ""); }
          else { wifiPassword = ""; keyboardDigit = 1; state = STATE_SETUP_WIFI_PASS; }
        }
      }
      else if (state == STATE_SETUP_WIFI_PASS) wifiPassword += String(keyboardDigit);
      else if (state == STATE_SETUP_ORDER) { state = STATE_SETTINGS; saveSettings(); }
      else if (state == STATE_SETUP_HELP) state = STATE_SETTINGS;
      else if (state == STATE_SETUP_BRIGHTNESS) { state = STATE_SETTINGS; saveSettings(); }
      else if (state == STATE_SETUP_NOTIFY) { state = STATE_SETTINGS; saveSettings(); }
      else if (state == STATE_SETUP_SLEEP) { state = STATE_SETTINGS; saveSettings(); }
      else if (state == STATE_SETUP_SYNC) state = STATE_SETTINGS;
      else if (state == STATE_SYNC_STATUS) state = STATE_SETTINGS;
      else if (state == STATE_SYNC_NOW) state = STATE_SYNC_STATUS;
      else if (state == STATE_MSG_MENU) state = STATE_MENU;
      else if (state == STATE_MSG_CHAT) state = STATE_MSG_MENU;
      else if (state == STATE_MSG_CHAT_VIEW) state = STATE_MSG_CHAT;
      else if (state == STATE_MSG_TEMPLATES) { if (templateCount > 0) addMessage(templates[currentTemplate], true); state = STATE_MSG_MENU; }
      else if (state == STATE_MSG_CUSTOM) { state = STATE_MSG_CUSTOM_LETTER; keyboardLetter = 0; }
      else if (state == STATE_MSG_CUSTOM_LETTER) {
        customMessage += String(LETTER_GROUPS[keyboardGroup][keyboardLetter]);
        state = STATE_MSG_CUSTOM; keyboardLetter = 0;
      }
      else if (state == STATE_NOTES_MENU) state = STATE_MENU;
      else if (state == STATE_NOTES_HISTORY) state = STATE_NOTES_MENU;
      else if (state == STATE_NOTES_VIEW) state = STATE_NOTES_HISTORY;
      else if (state == STATE_NOTES_ADD) { state = STATE_NOTES_ADD_LETTER; noteKeyboardLetter = 0; }
      else if (state == STATE_NOTES_ADD_LETTER) {
        if (noteKeyboardGroup < 5) noteText += String(LETTER_GROUPS[noteKeyboardGroup][noteKeyboardLetter]);
        else if (noteKeyboardGroup == 5) noteText += String(NOTE_DIGITS_GROUP[noteKeyboardLetter]);
        else noteText += " ";
        state = STATE_NOTES_ADD; noteKeyboardLetter = 0;
      }
      else if (state == STATE_CALCULATOR) { if (calcState == CALC_RESULT) calcReset(); else calcAdd(); }
      else if (state == STATE_GAMES_MENU) state = STATE_MENU;
    }
    shf = false;
  }

  if (state == STATE_GAME_PONG) {
    pongUpdate(); drawPong();
    if (!pongGameOver) {
      if (nextIsHeld) { pongPaddleLeftY -= 2; if (pongPaddleLeftY < 2) pongPaddleLeftY = 2; }
      if (sendIsHeld) { pongPaddleLeftY += 2; if (pongPaddleLeftY > 48) pongPaddleLeftY = 48; }
    }
  }
  else if (state == STATE_GAME_PONG_PAUSE) drawPongPause();
  else if (state == STATE_GAME_PONG_SETTINGS) drawPongSettings();
  else if (state == STATE_GAME_DODGER) {
    dodgerUpdate(); drawDodger();
    if (!dodgerGameOver) {
      if (nextIsHeld) { dodgerX -= 4; if (dodgerX < 0) dodgerX = 0; }
      if (sendIsHeld) { dodgerX += 4; if (dodgerX > 120) dodgerX = 120; }
    }
  }
  else if (state == STATE_GAME_DODGER_PAUSE) drawDodgerPause();
  else if (state == STATE_GAME_DINO) { dinoUpdate(); drawDino(); }
  else if (state == STATE_GAME_DINO_PAUSE) drawDinoPause();
  else if (state == STATE_SYNC_NOW) drawSyncNow();
  else if (state == STATE_SETUP_WIFI_CONNECTING) drawWifiConn();

  if (state == STATE_MENU) drawMainMenu();
  else if (state == STATE_STATS) drawStats();
  else if (state == STATE_AI_LIST) drawAIList();
  else if (state == STATE_AI_THINKING) drawThinking();
  else if (state == STATE_AI_RESPONSE) drawResponse();
  else if (state == STATE_TIME) drawTime();
  else if (state == STATE_NOTICE) drawNotice();
  else if (state == STATE_MAP_LIST) drawMapList();
  else if (state == STATE_SETTINGS) drawSettings();
  else if (state == STATE_SETUP_WIFI_LIST) drawWifiList();
  else if (state == STATE_SETUP_WIFI_PASS) drawKeyboard();
  else if (state == STATE_SETUP_ORDER) drawMenuOrder();
  else if (state == STATE_SETUP_HELP) drawHelp();
  else if (state == STATE_SETUP_BRIGHTNESS) drawBright();
  else if (state == STATE_SETUP_NOTIFY) drawNotif();
  else if (state == STATE_SETUP_SLEEP) drawSleep();
  else if (state == STATE_SETUP_SYNC) drawSyncSet();
  else if (state == STATE_SYNC_STATUS) drawSyncStat();
  else if (state == STATE_MSG_MENU) drawMsgMenu();
  else if (state == STATE_MSG_CHAT) drawMsgChat();
  else if (state == STATE_MSG_CHAT_VIEW) drawMsgChatView();
  else if (state == STATE_MSG_TEMPLATES) drawMsgTemplates();
  else if (state == STATE_MSG_CUSTOM) drawMsgCustom();
  else if (state == STATE_MSG_CUSTOM_LETTER) drawMsgCustomLetter();
  else if (state == STATE_CALCULATOR) drawCalculator();
  else if (state == STATE_NOTES_MENU) drawNotesMenu();
  else if (state == STATE_NOTES_ADD) drawNotesAdd();
  else if (state == STATE_NOTES_ADD_LETTER) drawNotesAddLetter();
  else if (state == STATE_NOTES_HISTORY) drawNotesHist();
  else if (state == STATE_NOTES_VIEW) drawNotesView();
  else if (state == STATE_GAMES_MENU) { if (gamesMenuItem == 3) drawGamesScore(); else drawGamesMenu(); }

  ESP.wdtFeed();
  delay(50);
}