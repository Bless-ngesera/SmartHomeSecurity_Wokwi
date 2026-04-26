// ================================================================
//   🏠 SMART HOME SECURITY SYSTEM — v24.0  (CLEAN + CHART-READY)
//   Parts A · B · C · Firebase
//
//   Improvements over v23:
//   ✓ COMPACT single-line output (no decorative boxes per event)
//   ✓ FIXED ThingSpeak HTTP -1 — explicit WiFiClient + setReuse(false)
//      + "Connection: close" + explicit client.stop()
//   ✓ LATCHED motion + door flags — so Field 1 / Field 3 charts
//      reflect real activity instead of staying flat at 0
//   ✓ ThingSpeak deferred until NTP synced (no BOOT+ entries)
//   ✓ Behaviour unchanged — only printing & sampling are smarter
//
//   Author: IUEA — Group 5 (Smart Home Security)
// ================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
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
int  thingspeakOk      = 0;
int  thingspeakFail    = 0;

// ───────── LATCHED chart flags (KEY FIX) ─────────
//   latchMotion  : TRUE between TS pushes if PIR fired
//   latchDoorOpen: TRUE between TS pushes if door was unlocked
//   Both reset after each successful TS push.
bool latchMotion   = false;
bool latchDoorOpen = false;

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
String cleanFirebaseHost();
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
void   testFirebaseManually();
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
void   pingDiagnostics();

// =================================================================
//   NTP
// =================================================================
void initNTP(bool verbose) {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC,
             NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
  if (verbose) Serial.print("⏰ NTP sync");
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo, 500) && attempts < 30) {
    if (verbose) Serial.print(".");
    attempts++; yield();
  }
  if (getLocalTime(&timeinfo)) {
    ntpSynced  = true;
    lastNTPSync = millis();
    if (verbose) {
      Serial.print(" ✅ "); Serial.print(getRealTimestamp());
      Serial.print(" ("); Serial.print(getDayName(timeinfo.tm_wday)); Serial.println(", UTC+3)");
    }
  } else {
    ntpSynced = false;
    if (verbose) Serial.println(" ❌ failed (will retry)");
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

String cleanFirebaseHost() {
  String host = String(FIREBASE_HOST);
  host.replace("https://", "");
  host.replace("http://",  "");
  if (host.endsWith("/")) host.remove(host.length() - 1);
  return host;
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
//   AI – weighted classifier
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
  if (intrusionCount >= 3)        { score += 0.30f; reasons += "Repeat attempts; "; }
  else if (intrusionCount == 2)   { score += 0.15f; reasons += "Second attempt; "; }
  if (!ntpSynced)                 { score += 0.05f; reasons += "Time unsynced; "; }
  if (score < 0.0f) score = 0.0f;
  if (score > 1.0f) score = 1.0f;
  e.aiConfidence = score;
  e.isSuspicious = (score >= 0.50f);
  e.aiReason     = (reasons.length() > 0) ? reasons : "Routine event";
}

// =================================================================
//   JSON helpers
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
//   FIREBASE  (compact one-line output, heap-allocated TLS)
// =================================================================
bool pushPayloadToFirebase(const String &payload) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("☁️  FB → Wi-Fi down (skipped)");
    firebaseFailCount++;
    return false;
  }

  String host = cleanFirebaseHost();
  String url  = "https://" + host + "/events.json";
  if (strlen(FIREBASE_SECRET) > 0) url += "?auth=" + String(FIREBASE_SECRET);

  WiFiClientSecure *client = new WiFiClientSecure();
  if (!client) { Serial.println("☁️  FB → out of memory"); firebaseFailCount++; return false; }
  client->setInsecure();
  client->setTimeout(10000);

  bool success = false;
  int  code    = 0;
  String resp;

  {
    HTTPClient https;
    https.setTimeout(10000);
    https.setReuse(false);
    yield();
    if (https.begin(*client, url)) {
      https.addHeader("Content-Type", "application/json");
      https.addHeader("Connection",   "close");
      yield();
      code = https.POST(payload);
      yield();
      resp = https.getString();
      if (resp.length() > 60) resp = resp.substring(0, 60) + "…";
      if (code == 200 || code == 201) { firebasePushCount++; success = true; }
      else firebaseFailCount++;
      https.end();
    } else {
      Serial.println("☁️  FB → begin() failed (check host/URL)");
      firebaseFailCount++;
    }
  }
  delete client;

  // ── ONE-LINE summary ──
  if (success) {
    Serial.print("☁️  FB ✅ HTTP "); Serial.print(code);
    Serial.print("  "); Serial.print(resp);
    Serial.print("  (heap "); Serial.print(ESP.getFreeHeap() / 1024); Serial.println("k)");
  } else if (code != 0) {
    Serial.print("☁️  FB ❌ HTTP "); Serial.print(code);
    Serial.print("  "); Serial.println(resp.length() ? resp : "(no body)");
  }
  return success;
}

void sendToFirebase(const Event &e) {
  String payload = eventToJson(e);
  if (!pushPayloadToFirebase(payload)) {
    if (queueCount < MAX_QUEUE) {
      pendingPayloads[queueCount++] = payload;
      Serial.print("📥 queued (depth "); Serial.print(queueCount); Serial.println(")");
    } else {
      for (int i = 0; i < MAX_QUEUE - 1; i++) pendingPayloads[i] = pendingPayloads[i + 1];
      pendingPayloads[MAX_QUEUE - 1] = payload;
      Serial.println("⚠️ queue full — oldest dropped");
    }
  }
}

void flushFirebaseQueue() {
  if (queueCount == 0) return;
  if (millis() - lastQueueFlush < QUEUE_FLUSH_INTERVAL) return;
  if (WiFi.status() != WL_CONNECTED) return;
  lastQueueFlush = millis();

  Serial.print("🔄 retry queue ("); Serial.print(queueCount); Serial.println(" pending)");
  if (pushPayloadToFirebase(pendingPayloads[0])) {
    for (int i = 0; i < queueCount - 1; i++) pendingPayloads[i] = pendingPayloads[i + 1];
    pendingPayloads[queueCount - 1] = "";
    queueCount--;
  }
}

void testFirebaseManually() {
  Serial.println("\n🧪 Manual Firebase health-check…");
  String testPayload =
    "{\"timestamp\":\"" + getRealTimestamp() + "\","
    "\"type\":\"TEST\","
    "\"detail\":\"Manual health-check from ESP32\","
    "\"device_id\":\"esp32_iuea_g5\","
    "\"location\":\"Kyaliwajala, Kampala\"}";
  bool ok = pushPayloadToFirebase(testPayload);
  Serial.println(ok ? "🎉 Firebase is RECEIVING data ✅\n"
                    : "❌ Firebase did NOT accept the test write\n");
}

// =================================================================
//   EVENT LOGGER  (compact 2-line output)
// =================================================================
void logEvent(const String &type, const String &detail, bool isAlarm) {
  Event e;
  e.eventType = type; e.detail = detail; e.isAlarm = isAlarm;
  getTimeFeatures(e);

  String actType = "SYSTEM";
  if      (type == "ALARM"  && detail.indexOf("RFID")   >= 0) actType = "UNAUTHORIZED_RFID";
  else if (type == "ALARM"  && detail.indexOf("Motion") >= 0) actType = "MOTION";
  else if (type == "ACCESS")                                   actType = "ACCESS_GRANTED";

  classifyActivity(e, actType);

  eventLog[eventHead] = e;
  eventHead = (eventHead + 1) % MAX_EVENTS;
  eventTotal++;

  // ── ONE line: timestamp | day | type | detail ──
  Serial.print("📊 ["); Serial.print(e.isoTimestamp); Serial.print(" "); Serial.print(e.dayName);
  Serial.print("] "); Serial.print(type); Serial.print(" \""); Serial.print(detail); Serial.println("\"");
  // ── ONE line: AI verdict ──
  Serial.print("   → "); Serial.print(e.isSuspicious ? "🚨 SUSPICIOUS" : "✅ NORMAL");
  Serial.print(" ("); Serial.print((int)(e.aiConfidence * 100)); Serial.print("%) — ");
  Serial.println(e.aiReason);

  sendToFirebase(e);
}

// =================================================================
//   BUZZER
// =================================================================
void buzzerInit() {
  ledcSetup(BUZZER_CHANNEL, TONE_HIGH, BUZZER_RES_BITS);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWriteTone(BUZZER_CHANNEL, 0);
}
void buzzerSirenStart() { buzzerOn = true; buzzerHighPhase = true; ledcWriteTone(BUZZER_CHANNEL, TONE_HIGH); }
void buzzerSirenStop()  { buzzerOn = false; ledcWriteTone(BUZZER_CHANNEL, 0); }
void buzzerSirenEffect() {
  if (!buzzerOn) return;
  if (millis() - lastToneChange < BUZZER_INTERVAL) return;
  lastToneChange = millis();
  buzzerHighPhase = !buzzerHighPhase;
  ledcWriteTone(BUZZER_CHANNEL, buzzerHighPhase ? TONE_HIGH : TONE_LOW);
}
void buzzerSuccessBeep() {
  ledcWriteTone(BUZZER_CHANNEL, TONE_OK); delay(180);
  ledcWriteTone(BUZZER_CHANNEL, 0);       delay(80);
  ledcWriteTone(BUZZER_CHANNEL, TONE_OK); delay(180);
  ledcWriteTone(BUZZER_CHANNEL, 0);
}

// =================================================================
//   SERVO  (latches latchDoorOpen on unlock)
// =================================================================
void setServoAngle(int angle) {
  currentServoAngle = angle;
  int pulseWidth = map(angle, 0, 180, 1000, 2000);
  digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(pulseWidth); digitalWrite(SERVO_PIN, LOW);
  if (angle == 90) latchDoorOpen = true;   // ← chart latch
}
void refreshServo() {
  if (millis() - lastServoPulse < SERVO_REFRESH_INTERVAL) return;
  lastServoPulse = millis();
  int pulseWidth = map(currentServoAngle, 0, 180, 1000, 2000);
  digitalWrite(SERVO_PIN, HIGH); delayMicroseconds(pulseWidth); digitalWrite(SERVO_PIN, LOW);
}

// =================================================================
//   Wi-Fi WATCHDOG
// =================================================================
void checkWiFiConnection() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL) return;
  lastWiFiCheck = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) { Serial.println("⚠️ Wi-Fi lost"); wifiConnected = false; }
    Serial.print("📡 reconnecting");
    WiFi.disconnect(); delay(80);
    WiFi.begin(ssid, pass);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) { delay(500); Serial.print("."); attempts++; }
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      Serial.print(" ✅ "); Serial.println(WiFi.localIP());
      if (!ntpSynced) initNTP(true);
    } else Serial.println(" ❌");
  } else if (!wifiConnected) {
    wifiConnected = true;
    if (!ntpSynced) initNTP(true);
  }
}

// =================================================================
//   THINGSPEAK  (fixed: explicit WiFiClient + setReuse(false))
//   Uses LATCHED motion/door so the charts show real activity
// =================================================================
void sendToThingSpeak() {
  if (millis() - lastThingSpeakUpdate < THINGSPEAK_UPDATE_INTERVAL) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ntpSynced) return;        // skip until clock is real (no BOOT+ entries)
  lastThingSpeakUpdate = millis();

  // ── KEY FIX: latched motion + door, so charts aren't always 0 ──
  // Motion : 1 if PIR fired since last push (or PIR currently HIGH)
  // Door   : 1 if door was unlocked at any point since last push
  int motionValue  = (latchMotion || digitalRead(PIR_PIN) == HIGH) ? 1 : 0;
  int doorValue    = (latchDoorOpen || currentServoAngle == 90)    ? 1 : 0;
  int alarmValue   = alarmActive ? 1 : 0;
  int eventTypeVal = alarmActive ? 2 : (accessGrantedCount > 0 ? 1 : 0);
  int threatLevel  = alarmActive ? 1 : 0;

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

  // ── KEY FIX: explicit WiFiClient prevents stale-socket HTTP -1 ──
  WiFiClient client;
  HTTPClient http;
  http.setReuse(false);
  http.setTimeout(8000);

  if (!http.begin(client, url)) {
    Serial.println("📡 TS → begin() failed");
    thingspeakFail++;
    client.stop();
    return;
  }
  http.addHeader("Connection", "close");

  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  client.stop();

  if (code == 200) {
    body.trim();
    Serial.print("📡 TS ✅ entry "); Serial.print(body);
    Serial.print("  [m=");  Serial.print(motionValue);
    Serial.print(" d=");    Serial.print(doorValue);
    Serial.print(" a=");    Serial.print(alarmValue);
    Serial.print(" int=");  Serial.print(intrusionCount);
    Serial.print(" acc=");  Serial.print(accessGrantedCount); Serial.println("]");
    thingspeakOk++;
    // reset latches now that ThingSpeak captured the events
    latchMotion   = false;
    latchDoorOpen = (currentServoAngle == 90);   // keep TRUE if still unlocked
  } else {
    Serial.print("📡 TS ❌ HTTP "); Serial.println(code);
    thingspeakFail++;
    // do NOT reset latches — try again on next cycle
  }
}

// =================================================================
//   RFID  (Wokwi serial-driven)
// =================================================================
void checkRFIDCommand() {
  if (millis() - lastRFIDCheck < RFID_CHECK_INTERVAL) return;
  lastRFIDCheck = millis();
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim(); input.toUpperCase();

  if      (input == "TAP GREEN") { Serial.println("🔘 GREEN CARD"); memcpy(readUID, authorisedUID, 4); cardPresent = true; }
  else if (input == "RESET" && alarmActive) stopAlarm();
  else if (input == "LOG")     printEventLog();
  else if (input == "EXPORT")  exportEventsForColab();
  else if (input == "STATUS")  printSystemStatus();
  else if (input == "HELP")    printHelp();
  else if (input == "TIME")    Serial.println("⏰ " + getRealTimestamp());
  else if (input == "SYNC")    initNTP(true);
  else if (input == "TEST FB") testFirebaseManually();
  else if (input == "PING")    pingDiagnostics();
  else if (input.length() > 0 && !alarmActive) {
    Serial.println("🔘 UNKNOWN CARD");
    byte fakeUID[] = {0x99, 0x88, 0x77, 0x66};
    memcpy(readUID, fakeUID, 4); cardPresent = true;
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
//   PRINTERS / DIAGNOSTICS  (boxed — only shown on demand)
// =================================================================
void printEventLog() {
  Serial.println("\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║              📋  SECURITY EVENT LOG                      ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝");
  if (eventTotal == 0) { Serial.println("(no events yet)\n"); return; }
  int count = (eventTotal < MAX_EVENTS) ? eventTotal : MAX_EVENTS;
  int start = (eventTotal < MAX_EVENTS) ? 0 : eventHead;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % MAX_EVENTS;
    Event &e = eventLog[idx];
    Serial.print("["); Serial.print(i + 1); Serial.print("] ");
    Serial.print(e.isoTimestamp); Serial.print(" | ");
    Serial.print(e.dayName);      Serial.print(" | ");
    Serial.print(e.eventType);    Serial.print(" — ");
    Serial.print(e.detail);
    Serial.print(" ⟶ ");
    Serial.print(e.isSuspicious ? "🚨 SUSPICIOUS" : "✅ NORMAL");
    Serial.print(" ("); Serial.print((int)(e.aiConfidence * 100)); Serial.println("%)");
  }
  Serial.println();
}

void printSystemStatus() {
  Serial.println("\n══════════════════════════════════════════════════════════");
  Serial.println("📊  SYSTEM STATUS");
  Serial.println("══════════════════════════════════════════════════════════");
  Serial.print("Real Time         : "); Serial.println(getRealTimestamp());
  Serial.print("NTP Synced        : "); Serial.println(ntpSynced ? "YES ✅" : "NO ❌");
  Serial.print("Wi-Fi             : "); Serial.println(wifiConnected ? "CONNECTED ✅" : "DISCONNECTED ❌");
  if (wifiConnected) { Serial.print("Local IP / RSSI   : "); Serial.print(WiFi.localIP()); Serial.print(" / "); Serial.print(WiFi.RSSI()); Serial.println(" dBm"); }
  Serial.print("Heap free / min   : "); Serial.print(ESP.getFreeHeap()); Serial.print(" / "); Serial.print(ESP.getMinFreeHeap()); Serial.println(" B");
  Serial.print("Firebase host     : "); Serial.println(cleanFirebaseHost());
  Serial.print("Firebase ✓ / ✗    : "); Serial.print(firebasePushCount); Serial.print(" / "); Serial.println(firebaseFailCount);
  Serial.print("ThingSpeak ✓ / ✗  : "); Serial.print(thingspeakOk); Serial.print(" / "); Serial.println(thingspeakFail);
  Serial.print("Retry queue depth : "); Serial.println(queueCount);
  Serial.print("Latched motion    : "); Serial.println(latchMotion ? "YES" : "no");
  Serial.print("Latched door open : "); Serial.println(latchDoorOpen ? "YES" : "no");
  Serial.print("Alarm Active      : "); Serial.println(alarmActive ? "YES 🚨" : "NO ✅");
  Serial.print("Door              : "); Serial.println(currentServoAngle == 90 ? "UNLOCKED 🔓" : "LOCKED 🔒");
  Serial.print("Total Intrusions  : "); Serial.println(intrusionCount);
  Serial.print("Total Access OK   : "); Serial.println(accessGrantedCount);
  Serial.print("Events Logged     : "); Serial.println(eventTotal);
  Serial.println("══════════════════════════════════════════════════════════\n");
}

void pingDiagnostics() {
  Serial.println("\n══════════════════════════════════════════════════════════");
  Serial.println("📶  PING DIAGNOSTICS");
  Serial.println("══════════════════════════════════════════════════════════");
  Serial.print("Wi-Fi          : "); Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED ✅" : "DOWN ❌");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("RSSI           : "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
    Serial.print("IP / Gateway   : "); Serial.print(WiFi.localIP()); Serial.print(" / "); Serial.println(WiFi.gatewayIP());
    Serial.print("DNS            : "); Serial.println(WiFi.dnsIP());
  }
  Serial.print("NTP            : "); Serial.println(ntpSynced ? "SYNCED ✅" : "NOT SYNCED ❌");
  Serial.print("Real Time      : "); Serial.println(getRealTimestamp());
  Serial.print("Firebase host  : "); Serial.println(cleanFirebaseHost());
  Serial.println("══════════════════════════════════════════════════════════\n");
}

void printHelp() {
  Serial.println("══════════════════════════════════════════════════════════");
  Serial.println("📌  COMMANDS");
  Serial.println("    TAP GREEN   → authorized access");
  Serial.println("    <any text>  → unauthorized → triggers alarm");
  Serial.println("    RESET       → stop active alarm");
  Serial.println("    LOG         → print event log with AI scores");
  Serial.println("    STATUS      → snapshot (heap, FB/TS counts, latches)");
  Serial.println("    EXPORT      → JSON dump for Colab");
  Serial.println("    TIME        → current real time");
  Serial.println("    SYNC        → force NTP resync");
  Serial.println("    PING        → Wi-Fi & NTP diagnostics");
  Serial.println("    TEST FB     → manual Firebase health-check");
  Serial.println("    HELP        → this menu");
  Serial.println("══════════════════════════════════════════════════════════");
}

void exportEventsForColab() {
  Serial.println("\n📤  COLAB AI-TRAINING EXPORT");
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
//   ALARM / ACCESS  (compact one-line triggers)
// =================================================================
void triggerAlarm(const String &reason) {
  if (alarmActive || accessGranted) return;
  alarmActive = true;
  alarmStartTime = millis();
  lastBlinkTime  = millis();
  lastToneChange = millis();
  intrusionCount++;
  latchMotion = true;          // ← ensures next TS push shows the motion
  Serial.print("\n🚨 INTRUSION #"); Serial.print(intrusionCount);
  Serial.print(" ["); Serial.print(getRealTimestamp()); Serial.print("] : ");
  Serial.println(reason);
  buzzerSirenStart();
  setServoAngle(0);
  logEvent("ALARM", reason, true);
}

void stopAlarm() {
  if (!alarmActive) return;
  alarmActive = false;
  digitalWrite(RED_LED, LOW);
  buzzerSirenStop();
  Serial.println("✅ Alarm cleared — system normal");
  logEvent("SYSTEM", "Alarm stopped — back to normal", false);
}

void grantAccess() {
  if (alarmActive) stopAlarm();
  accessGranted = true;
  accessGrantedCount++;
  unlockTimerActive = true;
  unlockStartTime   = millis();
  Serial.print("\n✅ ACCESS GRANTED ["); Serial.print(getRealTimestamp()); Serial.println("]");
  digitalWrite(GREEN_LED, HIGH);
  buzzerSuccessBeep();
  setServoAngle(90);           // ← triggers latchDoorOpen = true inside setServoAngle
  logEvent("ACCESS", "Authorized user — door unlocked", false);
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
//   SETUP
// =================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIR_PIN, INPUT);
  pinMode(SERVO_PIN, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED, OUTPUT);   pinMode(GREEN_LED, OUTPUT);
  digitalWrite(RED_LED, LOW); digitalWrite(GREEN_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW); digitalWrite(SERVO_PIN, LOW);
  buzzerInit();

  Serial.println("\n╔══════════════════════════════════════════════════════════╗");
  Serial.println("║   🏠   SMART HOME SECURITY  v24.0  ·  IUEA Group 5       ║");
  Serial.println("╚══════════════════════════════════════════════════════════╝");
  Serial.print("Heap : "); Serial.print(ESP.getFreeHeap()); Serial.print(" B  ·  FB : ");
  Serial.println(cleanFirebaseHost());

  Serial.print("📡 Wi-Fi");
  WiFi.begin(ssid, pass);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) { delay(500); Serial.print("."); attempts++; }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print(" ✅ "); Serial.print(WiFi.localIP());
    Serial.print(" ("); Serial.print(WiFi.RSSI()); Serial.println(" dBm)");
    initNTP(true);
  } else Serial.println(" ❌ (watchdog will retry)");

  // Self-test
  digitalWrite(GREEN_LED, HIGH); delay(200); digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED,   HIGH); delay(200); digitalWrite(RED_LED,   LOW);
  buzzerSuccessBeep();
  setServoAngle(90); delay(300); setServoAngle(0);
  latchDoorOpen = false;          // self-test door movement shouldn't pollute first chart point

  printHelp();
  Serial.println("🔒 Door LOCKED  ·  System ARMED\n");
}

// =================================================================
//   LOOP
// =================================================================
void loop() {
  refreshServo();
  checkWiFiConnection();
  resyncNTPIfNeeded();

  if (!firstBootLogged && wifiConnected && ntpSynced && millis() > 5000) {
    firstBootLogged = true;
    logEvent("SYSTEM", "System started — armed and ready", false);
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
    else                       triggerAlarm("Invalid / unauthorized RFID tag");
    cardPresent = false;
  }

  delay(10);
}