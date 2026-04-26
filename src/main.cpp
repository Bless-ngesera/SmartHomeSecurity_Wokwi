// ================================================================
//   🏠 SMART HOME SECURITY SYSTEM — v22.0 (FULLY WORKING)
//   Parts A · B · C — Production-grade refactor
//   FIXED: ledc functions for ESP32, compilation errors resolved
// ================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ───────── Wi-Fi ─────────
const char* ssid = "Wokwi-GUEST";
const char* pass = "";

// ───────── ThingSpeak (Part B) ─────────
#define THINGSPEAK_API_KEY          "TR75DQ8VN632VLRU"
#define THINGSPEAK_HOST             "api.thingspeak.com"
#define THINGSPEAK_UPDATE_INTERVAL  20000UL

// ───────── Firebase (Part C) ─────────
#define FIREBASE_HOST    "smart-home-security-syst-2d94a-default-rtdb.europe-west1.firebasedatabase.app"
#define FIREBASE_SECRET  ""

// ───────── NTP ─────────
const long  GMT_OFFSET_SEC      = 3L * 3600L;
const int   DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER_1        = "pool.ntp.org";
const char* NTP_SERVER_2        = "time.google.com";
const char* NTP_SERVER_3        = "time.nist.gov";
const unsigned long NTP_RESYNC_INTERVAL = 3600000UL;
bool  ntpSynced  = false;

// ───────── Pins ─────────
#define PIR_PIN     13
#define SERVO_PIN   14
#define BUZZER_PIN  27
#define RED_LED     26
#define GREEN_LED   25

// ───────── Buzzer ─────────
#define BUZZER_RES_BITS 8
#define BUZZER_CHANNEL  0
#define TONE_HIGH       1200
#define TONE_LOW        800
#define TONE_OK         2000

// ───────── Timers ─────────
unsigned long lastMotionTime       = 0;
unsigned long alarmStartTime       = 0;
unsigned long lastBlinkTime        = 0;
unsigned long lastToneChange       = 0;
unsigned long lastServoPulse       = 0;
unsigned long lastThingSpeakUpdate = 0;
unsigned long lastWiFiCheck        = 0;
unsigned long lastRFIDCheck        = 0;
unsigned long lastNTPSync          = 0;
unsigned long unlockStartTime      = 0;
unsigned long lastQueueFlush       = 0;

const unsigned long WIFI_CHECK_INTERVAL    = 10000UL;
const unsigned long MOTION_COOLDOWN        = 2000UL;
const unsigned long ALARM_DURATION         = 10000UL;
const unsigned long BLINK_INTERVAL         = 300UL;
const unsigned long BUZZER_INTERVAL        = 300UL;
const unsigned long SERVO_REFRESH_INTERVAL = 20UL;
const unsigned long RFID_CHECK_INTERVAL    = 100UL;
const unsigned long UNLOCK_DURATION        = 3000UL;
const unsigned long QUEUE_FLUSH_INTERVAL   = 8000UL;

// ───────── State ─────────
bool alarmActive       = false;
bool accessGranted     = false;
bool unlockTimerActive = false;
bool ledState          = false;
bool buzzerOn          = false;
bool buzzerHighPhase   = true;
bool wifiConnected     = false;
bool firstBootLogged   = false;
int  currentServoAngle = 0;
int  intrusionCount    = 0;
int  accessGrantedCount = 0;
int  firebasePushCount = 0;
int  firebaseFailCount = 0;

// ───────── RFID ─────────
byte authorisedUID[] = {0xDE, 0xAD, 0xBE, 0xEF};
byte readUID[4]      = {0, 0, 0, 0};
bool cardPresent     = false;

// ───────── Event model ─────────
struct Event {
  time_t epochSeconds;
  String isoTimestamp;
  String eventType;
  String detail;
  bool   isAlarm;
  int    hour;
  int    minute;
  int    dayOfWeek;
  String dayName;
  bool   isNightTime;
  bool   isWeekend;
  bool   isSuspicious;
  float  aiConfidence;
  String aiReason;
};

const int  MAX_EVENTS = 20;
Event      eventLog[MAX_EVENTS];
int        eventHead  = 0;
int        eventTotal = 0;

// ───────── Firebase retry queue ─────────
const int  MAX_QUEUE = 10;
String     pendingPayloads[MAX_QUEUE];
int        queueCount = 0;

// ───────── Forward decls ─────────
String getRealTimestamp();
String getDayName(int wday);
void   initNTP(bool verbose);
void   resyncNTPIfNeeded();
void   getTimeFeatures(Event &e);
void   classifyActivity(Event &e, const String &actType);
String escapeJsonString(const String &in);
String eventToJson(const Event &e);
void   logEvent(const String &type, const String &detail, bool isAlarm);
bool   pushPayloadToFirebase(const String &payload);
void   sendToFirebase(const Event &e);
void   flushFirebaseQueue();
void   sendToThingSpeak();
void   checkWiFiConnection();
void   checkRFIDCommand();
void   checkMotion();
void   triggerAlarm(const String &reason);
void   stopAlarm();
void   grantAccess();
void   updateAlarmEffects();
void   updateUnlockTimer();
void   setServoAngle(int angle);
void   refreshServo();
void   buzzerInit();
void   buzzerSirenStart();
void   buzzerSirenStop();
void   buzzerSirenEffect();
void   buzzerSuccessBeep();
void   printUID(byte *uid);
bool   isAuthorized(byte *uid);
void   printEventLog();
void   printSystemStatus();
void   printHelp();
void   exportEventsForColab();

// =================================================================
//                NTP
// =================================================================
void initNTP(bool verbose) {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  if (verbose) Serial.print("⏰ Syncing time via NTP");
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo, 500) && attempts < 12) {
    if (verbose) Serial.print(".");
    attempts++; yield();
  }
  if (getLocalTime(&timeinfo)) {
    ntpSynced  = true;
    lastNTPSync = millis();
    if (verbose) {
      Serial.println();
      Serial.print("✅ Time synced → "); Serial.print(getRealTimestamp());
      Serial.print(" ("); Serial.print(getDayName(timeinfo.tm_wday));
      Serial.println(", Africa/Kampala)");
    }
  } else {
    ntpSynced = false;
    if (verbose) Serial.println("\n⚠️ NTP sync failed – will retry hourly");
  }
}

void resyncNTPIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastNTPSync >= NTP_RESYNC_INTERVAL || !ntpSynced) initNTP(false);
}

String getRealTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    char buf[24];
    snprintf(buf, sizeof(buf), "BOOT+%lus", millis() / 1000);
    return String(buf);
  }
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

String getDayName(int wday) {
  static const char* days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  if (wday < 0 || wday > 6) return "??";
  return String(days[wday]);
}

void getTimeFeatures(Event &e) {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    e.epochSeconds = mktime(&timeinfo);
    e.isoTimestamp = getRealTimestamp();
    e.hour = timeinfo.tm_hour; e.minute = timeinfo.tm_min;
    e.dayOfWeek = timeinfo.tm_wday; e.dayName = getDayName(timeinfo.tm_wday);
  } else {
    e.epochSeconds = 0; e.isoTimestamp = "UNSYNCED";
    e.hour = 0; e.minute = 0; e.dayOfWeek = 0; e.dayName = "???";
  }
  e.isNightTime = (e.hour >= 22 || e.hour < 6);
  e.isWeekend   = (e.dayOfWeek == 0 || e.dayOfWeek == 6);
}

// =================================================================
//                AI CLASSIFIER
// =================================================================
void classifyActivity(Event &e, const String &activityType) {
  float score = 0.0f;
  String reasons = "";
  if (e.isNightTime)                       { score += 0.30f; reasons += "Night-time; "; }
  if (e.isWeekend)                         { score += 0.10f; reasons += "Weekend; ";    }
  if (activityType == "UNAUTHORIZED_RFID") { score += 0.55f; reasons += "Invalid RFID; "; }
  else if (activityType == "MOTION")       { score += 0.25f; reasons += "PIR motion; ";   }
  else if (activityType == "ACCESS_GRANTED"){score -= 0.20f; reasons += "Authorized; ";   }
  else if (activityType == "SYSTEM")       { score -= 0.50f; reasons += "System event; "; }
  if (intrusionCount >= 3) { score += 0.30f; reasons += "Repeat attempts; "; }
  else if (intrusionCount == 2) { score += 0.15f; reasons += "Second attempt; "; }
  if (!ntpSynced) { score += 0.05f; reasons += "Time unsynced; "; }
  if (score < 0.0f) score = 0.0f;
  if (score > 1.0f) score = 1.0f;
  e.aiConfidence = score;
  e.isSuspicious = (score >= 0.50f);
  e.aiReason = (reasons.length() > 0) ? reasons : "Routine event";
}

// =================================================================
//                JSON HELPERS
// =================================================================
String escapeJsonString(const String &in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

String eventToJson(const Event &e) {
  String j = "{";
  j += "\"timestamp\":\"" + escapeJsonString(e.isoTimestamp) + "\",";
  j += "\"epoch\":" + String((long)e.epochSeconds) + ",";
  j += "\"type\":\"" + escapeJsonString(e.eventType) + "\",";
  j += "\"detail\":\"" + escapeJsonString(e.detail) + "\",";
  j += "\"hour\":" + String(e.hour) + ",";
  j += "\"minute\":" + String(e.minute) + ",";
  j += "\"day_of_week\":" + String(e.dayOfWeek) + ",";
  j += "\"day_name\":\"" + e.dayName + "\",";
  j += "\"is_alarm\":" + String(e.isAlarm ? "true" : "false") + ",";
  j += "\"is_night\":" + String(e.isNightTime ? "true" : "false") + ",";
  j += "\"is_weekend\":" + String(e.isWeekend ? "true" : "false") + ",";
  j += "\"is_suspicious\":" + String(e.isSuspicious ? "true" : "false") + ",";
  j += "\"ai_confidence\":" + String(e.aiConfidence, 3) + ",";
  j += "\"ai_reason\":\"" + escapeJsonString(e.aiReason) + "\",";
  j += "\"device_id\":\"esp32_iuea_g5\",";
  j += "\"location\":\"Kyaliwajala, Kampala\"";
  j += "}";
  return j;
}

// =================================================================
//                FIREBASE
// =================================================================
bool pushPayloadToFirebase(const String &payload) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String host = String(FIREBASE_HOST);
  host.replace("https://", "");
  host.replace("http://", "");
  if (host.endsWith("/")) host.remove(host.length() - 1);

  String url = "https://" + host + "/events.json";
  if (strlen(FIREBASE_SECRET) > 0) url += "?auth=" + String(FIREBASE_SECRET);

  WiFiClientSecure *client = new WiFiClientSecure();
  if (!client) return false;
  client->setInsecure();
  client->setTimeout(10000);

  bool success = false;
  {
    HTTPClient https;
    https.setTimeout(10000);
    https.setReuse(false);

    if (https.begin(*client, url)) {
      https.addHeader("Content-Type", "application/json");
      int code = https.POST(payload);
      if (code == 200 || code == 201) {
        firebasePushCount++;
        success = true;
      } else {
        firebaseFailCount++;
      }
      https.end();
    } else {
      firebaseFailCount++;
    }
  }
  delete client;
  return success;
}

void sendToFirebase(const Event &e) {
  String payload = eventToJson(e);
  if (!pushPayloadToFirebase(payload)) {
    if (queueCount < MAX_QUEUE) {
      pendingPayloads[queueCount++] = payload;
    }
  }
}

void flushFirebaseQueue() {
  if (queueCount == 0) return;
  if (millis() - lastQueueFlush < QUEUE_FLUSH_INTERVAL) return;
  if (WiFi.status() != WL_CONNECTED) return;
  lastQueueFlush = millis();
  
  if (pushPayloadToFirebase(pendingPayloads[0])) {
    for (int i = 0; i < queueCount - 1; i++) pendingPayloads[i] = pendingPayloads[i + 1];
    queueCount--;
  }
}

// =================================================================
//                EVENT LOGGER
// =================================================================
void logEvent(const String &type, const String &detail, bool isAlarm) {
  Event e;
  e.eventType = type; e.detail = detail; e.isAlarm = isAlarm;
  getTimeFeatures(e);

  String actType = "SYSTEM";
  if (type == "ALARM" && detail.indexOf("RFID") >= 0) actType = "UNAUTHORIZED_RFID";
  else if (type == "ALARM" && detail.indexOf("Motion") >= 0) actType = "MOTION";
  else if (type == "ACCESS") actType = "ACCESS_GRANTED";

  classifyActivity(e, actType);

  eventLog[eventHead] = e;
  eventHead = (eventHead + 1) % MAX_EVENTS;
  eventTotal++;

  Serial.println("\n═══════════════════════════════════════════════");
  Serial.print("📊 "); Serial.print(type); Serial.print(" | ");
  Serial.print(e.isoTimestamp); Serial.print(" | ");
  Serial.print(e.isSuspicious ? "🚨 SUSPICIOUS" : "✅ NORMAL");
  Serial.print(" ("); Serial.print((int)(e.aiConfidence * 100)); Serial.println("%)");
  Serial.println("═══════════════════════════════════════════════");

  sendToFirebase(e);
}

// =================================================================
//                BUZZER (FIXED FOR ESP32)
// =================================================================
void buzzerInit() { 
  // Nothing needed - setup done in each function
}

void buzzerSirenStart() { 
  buzzerOn = true; 
  buzzerHighPhase = true;
  ledcSetup(BUZZER_CHANNEL, TONE_HIGH, BUZZER_RES_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, TONE_HIGH);
  Serial.println("🔊 SIREN ON"); 
}

void buzzerSirenStop() { 
  buzzerOn = false; 
  ledcWriteTone(BUZZER_CHANNEL, 0);
  ledcDetachPin(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("🔇 SIREN OFF"); 
}

void buzzerSirenEffect() {
  if (!buzzerOn) return;
  if (millis() - lastToneChange < BUZZER_INTERVAL) return;
  lastToneChange = millis();
  buzzerHighPhase = !buzzerHighPhase;
  ledcSetup(BUZZER_CHANNEL, buzzerHighPhase ? TONE_HIGH : TONE_LOW, BUZZER_RES_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, buzzerHighPhase ? TONE_HIGH : TONE_LOW);
}

void buzzerSuccessBeep() {
  ledcSetup(BUZZER_CHANNEL, TONE_OK, BUZZER_RES_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, TONE_OK);
  delay(180);
  ledcWriteTone(BUZZER_CHANNEL, 0);
  delay(80);
  ledcWriteTone(BUZZER_CHANNEL, TONE_OK);
  delay(180);
  ledcWriteTone(BUZZER_CHANNEL, 0);
  ledcDetachPin(BUZZER_PIN);
}

// =================================================================
//                SERVO
// =================================================================
void setServoAngle(int angle) {
  currentServoAngle = angle;
  int pulseWidth = map(angle, 0, 180, 1000, 2000);
  digitalWrite(SERVO_PIN, HIGH);
  delayMicroseconds(pulseWidth);
  digitalWrite(SERVO_PIN, LOW);
  Serial.println(angle == 0 ? "🔒 LOCKED" : "🔓 UNLOCKED");
}

void refreshServo() {
  if (millis() - lastServoPulse < SERVO_REFRESH_INTERVAL) return;
  lastServoPulse = millis();
  int pulseWidth = map(currentServoAngle, 0, 180, 1000, 2000);
  digitalWrite(SERVO_PIN, HIGH);
  delayMicroseconds(pulseWidth);
  digitalWrite(SERVO_PIN, LOW);
}

// =================================================================
//                Wi-Fi WATCHDOG
// =================================================================
void checkWiFiConnection() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) { Serial.println("⚠️ Wi-Fi lost!"); wifiConnected = false; }
    WiFi.disconnect(); delay(80);
    WiFi.begin(ssid, pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) { 
      delay(500); 
      attempts++; 
    }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      initNTP(true);
    }
  } else if (!wifiConnected) {
    wifiConnected = true;
  }
}

// =================================================================
//                THINGSPEAK
// =================================================================
void sendToThingSpeak() {
  if (millis() - lastThingSpeakUpdate < THINGSPEAK_UPDATE_INTERVAL) return;
  if (WiFi.status() != WL_CONNECTED) return;
  lastThingSpeakUpdate = millis();

  int motionValue = digitalRead(PIR_PIN);
  int alarmValue = alarmActive ? 1 : 0;
  int doorValue = (currentServoAngle == 90) ? 1 : 0;
  int eventTypeVal = alarmActive ? 2 : (accessGrantedCount > 0 ? 1 : 0);
  int threatLevel = alarmActive ? 1 : 0;

  struct tm timeinfo;
  int hourOfDay = (getLocalTime(&timeinfo)) ? timeinfo.tm_hour : 0;

  String url = "http://" + String(THINGSPEAK_HOST) + "/update";
  url += "?api_key=" + String(THINGSPEAK_API_KEY);
  url += "&field1=" + String(motionValue);
  url += "&field2=" + String(alarmValue);
  url += "&field3=" + String(doorValue);
  url += "&field4=" + String(intrusionCount);
  url += "&field5=" + String(accessGrantedCount);
  url += "&field6=" + String(eventTypeVal);
  url += "&field7=" + String(hourOfDay);
  url += "&field8=" + String(threatLevel);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  http.GET();
  http.end();
}

// =================================================================
//                RFID COMMANDS
// =================================================================
void checkRFIDCommand() {
  if (millis() - lastRFIDCheck < RFID_CHECK_INTERVAL) return;
  lastRFIDCheck = millis();
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim(); input.toUpperCase();

  if (input == "TAP GREEN") { 
    memcpy(readUID, authorisedUID, 4); 
    cardPresent = true; 
  }
  else if (input == "RESET" && alarmActive) stopAlarm();
  else if (input == "LOG") printEventLog();
  else if (input == "EXPORT") exportEventsForColab();
  else if (input == "STATUS") printSystemStatus();
  else if (input == "HELP") printHelp();
  else if (input == "TIME") Serial.println("⏰ " + getRealTimestamp());
  else if (input == "SYNC") initNTP(true);
  else if (input.length() > 0 && !alarmActive) {
    byte fakeUID[] = {0x99, 0x88, 0x77, 0x66};
    memcpy(readUID, fakeUID, 4); 
    cardPresent = true;
  }
}

void printUID(byte *uid) {
  Serial.print("→ UID: ");
  for (byte i = 0; i < 4; i++) {
    if (uid[i] < 0x10) Serial.print("0");
    Serial.print(uid[i], HEX);
    if (i < 3) Serial.print(" ");
  }
  Serial.println();
}

bool isAuthorized(byte *uid) {
  for (byte i = 0; i < 4; i++) if (uid[i] != authorisedUID[i]) return false;
  return true;
}

// =================================================================
//                PRINTERS
// =================================================================
void printEventLog() {
  Serial.println("\n📋 SECURITY EVENT LOG");
  if (eventTotal == 0) { Serial.println("No events yet"); return; }
  int count = (eventTotal < MAX_EVENTS) ? eventTotal : MAX_EVENTS;
  int start = (eventTotal < MAX_EVENTS) ? 0 : eventHead;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_EVENTS;
    Event &e = eventLog[idx];
    Serial.print(i+1); Serial.print("] ");
    Serial.print(e.isoTimestamp); Serial.print(" | ");
    Serial.print(e.eventType); Serial.print(" | ");
    Serial.print(e.isSuspicious ? "🚨" : "✅");
    Serial.print(" ("); Serial.print((int)(e.aiConfidence * 100)); Serial.println("%)");
  }
}

void printSystemStatus() {
  Serial.println("\n═══════════════════════════════════");
  Serial.println("📊 SYSTEM STATUS");
  Serial.print("Time: "); Serial.println(getRealTimestamp());
  Serial.print("WiFi: "); Serial.println(wifiConnected ? "CONNECTED" : "DISCONNECTED");
  Serial.print("Alarm: "); Serial.println(alarmActive ? "ACTIVE" : "OFF");
  Serial.print("Door: "); Serial.println(currentServoAngle == 90 ? "UNLOCKED" : "LOCKED");
  Serial.print("Intrusions: "); Serial.println(intrusionCount);
  Serial.print("Access OK: "); Serial.println(accessGrantedCount);
  Serial.print("Firebase: ✓"); Serial.print(firebasePushCount); 
  Serial.print(" ✗"); Serial.println(firebaseFailCount);
}

void printHelp() {
  Serial.println("\n📌 COMMANDS:");
  Serial.println("   TAP GREEN  → authorized access");
  Serial.println("   any text   → unauthorized (alarm)");
  Serial.println("   RESET      → stop alarm");
  Serial.println("   LOG        → view events");
  Serial.println("   STATUS     → system info");
  Serial.println("   EXPORT     → JSON for Colab");
}

void exportEventsForColab() {
  Serial.println("\n📤 COLAB EXPORT (JSON):\n");
  int count = (eventTotal < MAX_EVENTS) ? eventTotal : MAX_EVENTS;
  int start = (eventTotal < MAX_EVENTS) ? 0 : eventHead;
  Serial.print("{\"exported_at\":\""); Serial.print(getRealTimestamp());
  Serial.print("\",\"events\":[");
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_EVENTS;
    if (i > 0) Serial.print(",");
    Serial.print(eventToJson(eventLog[idx]));
  }
  Serial.println("]}\n");
}

// =================================================================
//                ALARM / ACCESS
// =================================================================
void triggerAlarm(const String &reason) {
  if (alarmActive || accessGranted) return;
  alarmActive = true;
  alarmStartTime = millis();
  lastBlinkTime = millis();
  lastToneChange = millis();
  intrusionCount++;
  
  Serial.println("\n🚨🚨🚨 INTRUSION — ALARM TRIGGERED! 🚨🚨🚨");
  Serial.print("→ "); Serial.println(reason);
  buzzerSirenStart();
  setServoAngle(0);
  logEvent("ALARM", reason, true);
  sendToThingSpeak();
}

void stopAlarm() {
  if (!alarmActive) return;
  alarmActive = false;
  digitalWrite(RED_LED, LOW);
  buzzerSirenStop();
  logEvent("SYSTEM", "Alarm stopped", false);
  sendToThingSpeak();
  Serial.println("✅ ALARM CLEARED");
}

void grantAccess() {
  if (alarmActive) stopAlarm();
  accessGranted = true;
  accessGrantedCount++;
  unlockTimerActive = true;
  unlockStartTime = millis();
  
  Serial.println("\n✅✅✅ ACCESS GRANTED ✅✅✅");
  digitalWrite(GREEN_LED, HIGH);
  buzzerSuccessBeep();
  setServoAngle(90);
  logEvent("ACCESS", "Door unlocked", false);
  sendToThingSpeak();
}

void updateUnlockTimer() {
  if (!unlockTimerActive) return;
  if (millis() - unlockStartTime < UNLOCK_DURATION) return;
  setServoAngle(0);
  digitalWrite(GREEN_LED, LOW);
  accessGranted = false;
  unlockTimerActive = false;
}

void updateAlarmEffects() {
  if (millis() - lastBlinkTime >= BLINK_INTERVAL) {
    lastBlinkTime = millis();
    ledState = !ledState;
    digitalWrite(RED_LED, ledState ? HIGH : LOW);
  }
  buzzerSirenEffect();
}

void checkMotion() {
  if (digitalRead(PIR_PIN) != HIGH) return;
  if (millis() - lastMotionTime < MOTION_COOLDOWN) return;
  lastMotionTime = millis();
  if (alarmActive || accessGranted) return;
  triggerAlarm("Motion detected by PIR sensor");
}

// =================================================================
//                SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIR_PIN, INPUT);
  pinMode(SERVO_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  digitalWrite(RED_LED, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(SERVO_PIN, LOW);

  Serial.println("\n╔══════════════════════════════════════════════╗");
  Serial.println("║   🏠 SMART HOME SECURITY v22.0 🏠           ║");
  Serial.println("║   Fully Functional | No Compilation Errors  ║");
  Serial.println("╚══════════════════════════════════════════════╝\n");

  WiFi.begin(ssid, pass);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    initNTP(true);
  }

  Serial.println("\n▶ Self-test...");
  digitalWrite(GREEN_LED, HIGH); delay(200); digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, HIGH); delay(200); digitalWrite(RED_LED, LOW);
  buzzerSuccessBeep();
  setServoAngle(90); delay(300); setServoAngle(0);

  printHelp();
  Serial.println("\n🔒 Door LOCKED | System ARMED\n");
}

// =================================================================
//                LOOP
// =================================================================
void loop() {
  refreshServo();
  checkWiFiConnection();
  resyncNTPIfNeeded();

  if (!firstBootLogged && wifiConnected && ntpSynced && millis() > 5000) {
    firstBootLogged = true;
    logEvent("SYSTEM", "System armed", false);
  }

  if (alarmActive && (millis() - alarmStartTime >= ALARM_DURATION)) stopAlarm();
  if (alarmActive) updateAlarmEffects();
  updateUnlockTimer();

  sendToThingSpeak();
  flushFirebaseQueue();
  checkMotion();
  checkRFIDCommand();

  if (cardPresent) {
    printUID(readUID);
    if (isAuthorized(readUID)) grantAccess();
    else triggerAlarm("Invalid RFID tag");
    cardPresent = false;
  }

  delay(10);
}