#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESP_Mail_Client.h>
#include <time.h>

/* ================= WIFI & GMAIL ================= */
const char *ssid = "simon";
const char *password = "SIMON1234";

#define SMTP_HOST       "smtp.gmail.com"
#define SMTP_PORT       465
#define AUTHOR_EMAIL    "rustusjunior@gmail.com"
#define APP_PASSWORD    "erue dlxm jpqc pnba"

/* ================= PINS ================= */
#define TRIG 5
#define ECHO 18
#define RELAY 17
#define DRAIN_RELAY 16
#define BUZZER 19

/* ================= OBJECTS ================= */
AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60 * 1000);
SMTPSession smtp;

/* ================= VARIABLES ================= */
float tankHeight = 19.0;
float waterLevelPercent = 0;
float smoothedLevel = 0; // Added for EMA Filtering

String tankStatus = "UNKNOWN";
String pumpStatus = "OFF";
String drainStatus = "OFF";
String lastFilledTime = "--:--:--";
int monthlyFills = 0;
bool alreadyCounted = false;
bool drainEnabled = false;
bool drainCycleArmed = false;

unsigned long lastMeasureTime = 0;
const unsigned long measureInterval = 40;
const float fillStartPercent = 20.0f;
const float fillStopPercent = 80.0f;
const float drainStopPercent = 35.0f;
const float sensorMaxDistanceCm = 30.0f;
const unsigned long echoTimeoutUs = 4000UL;
const float idleRiseBlend = 0.18f;
const float fillRiseBlend = 0.34f;
const float idleFallBlend = 0.05f;
const float drainFallBlend = 0.18f;
const float maxRiseStepPercent = 1.4f;
const float maxFallStepPercent = 0.30f;
const float maxDrainFallStepPercent = 0.85f;
const float fullHoldPercent = 79.8f;
const float fullReleasePercent = 74.0f;
const int fullReleaseSamples = 8;
float lastValidDistanceCm = tankHeight;
bool fullLevelLatched = false;
int fullReleaseCounter = 0;

/* ================= FAST JSON CACHE ================= */
char dataJson[1800];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

/* ================= EMAIL QUEUE (NON-BLOCKING) ================= */
volatile bool emailPending = false;
String pendingMsg = "";

/* ================= LIVE HARDWARE LOG ================= */
static const int LOG_SIZE = 20;
static const int MONTHLY_LOG_SIZE = 240;

struct LogEntry {
  char ts[16];
  char action[40];
};

struct MonthlyLogEntry {
  char stamp[24];
  char action[40];
};

LogEntry logBuf[LOG_SIZE];
volatile int logHead = 0;
volatile int logCount = 0;
MonthlyLogEntry monthlyLogBuf[MONTHLY_LOG_SIZE];
volatile int monthlyLogHead = 0;
volatile int monthlyLogCount = 0;
int activeLogMonth = -1;
int activeLogYear = -1;

void formatLogTimestamps(char *timeOnly, size_t timeSize, char *dateTime, size_t dateSize, int *yearOut = nullptr, int *monthOut = nullptr) {
  const unsigned long epoch = timeClient.getEpochTime();
  if (epoch > 100000UL) {
    time_t localEpoch = static_cast<time_t>(epoch);
    struct tm tmInfo;
    gmtime_r(&localEpoch, &tmInfo);
    snprintf(timeOnly, timeSize, "%02d:%02d:%02d", tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
    snprintf(dateTime, dateSize, "%04d-%02d-%02d %02d:%02d:%02d",
             tmInfo.tm_year + 1900, tmInfo.tm_mon + 1, tmInfo.tm_mday,
             tmInfo.tm_hour, tmInfo.tm_min, tmInfo.tm_sec);
    if (yearOut) *yearOut = tmInfo.tm_year;
    if (monthOut) *monthOut = tmInfo.tm_mon;
    return;
  }

  const String now = timeClient.getFormattedTime();
  snprintf(timeOnly, timeSize, "%s", now.c_str());
  snprintf(dateTime, dateSize, "0000-00-00 %s", now.c_str());
  if (yearOut) *yearOut = -1;
  if (monthOut) *monthOut = -1;
}

bool syncMonthlyWindow() {
  const unsigned long epoch = timeClient.getEpochTime();
  if (epoch <= 100000UL) return false;

  time_t localEpoch = static_cast<time_t>(epoch);
  struct tm tmInfo;
  gmtime_r(&localEpoch, &tmInfo);

  bool changed = false;
  taskENTER_CRITICAL(&mux);
  if (activeLogYear != tmInfo.tm_year || activeLogMonth != tmInfo.tm_mon) {
    activeLogYear = tmInfo.tm_year;
    activeLogMonth = tmInfo.tm_mon;
    monthlyLogHead = 0;
    monthlyLogCount = 0;
    monthlyFills = 0;
    changed = true;
  }
  taskEXIT_CRITICAL(&mux);
  return changed;
}

void addLog(const char *action) {
  char ts[16];
  char stamp[24];
  int logYear = -1;
  int logMonth = -1;
  formatLogTimestamps(ts, sizeof(ts), stamp, sizeof(stamp), &logYear, &logMonth);

  taskENTER_CRITICAL(&mux);
  if (logYear >= 0 && logMonth >= 0 && (activeLogYear != logYear || activeLogMonth != logMonth)) {
    activeLogYear = logYear;
    activeLogMonth = logMonth;
    monthlyLogHead = 0;
    monthlyLogCount = 0;
    monthlyFills = 0;
  }
  snprintf(logBuf[logHead].ts, sizeof(logBuf[logHead].ts), "%s", ts);
  snprintf(logBuf[logHead].action, sizeof(logBuf[logHead].action), "%s", action);
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;

  snprintf(monthlyLogBuf[monthlyLogHead].stamp, sizeof(monthlyLogBuf[monthlyLogHead].stamp), "%s", stamp);
  snprintf(monthlyLogBuf[monthlyLogHead].action, sizeof(monthlyLogBuf[monthlyLogHead].action), "%s", action);
  monthlyLogHead = (monthlyLogHead + 1) % MONTHLY_LOG_SIZE;
  if (monthlyLogCount < MONTHLY_LOG_SIZE) monthlyLogCount++;
  taskEXIT_CRITICAL(&mux);
}

/* ================= SMTP CALLBACK ================= */
void smtpCallback(SMTP_Status status) {
  Serial.println(status.info());
}

void queueEmailAlert(const char *statusMsg) {
  pendingMsg = statusMsg;
  emailPending = true;
}

/* ================= SEND GMAIL ALERT ================= */
void sendGmailAlert(const String &statusMsg) {
  if (WiFi.status() != WL_CONNECTED) {
    addLog("Email failed: WiFi down");
    return;
  }
  ESP_Mail_Session session;
  session.server.host_name = SMTP_HOST;
  session.server.port = SMTP_PORT;
  session.login.email = AUTHOR_EMAIL;
  session.login.password = APP_PASSWORD;
  session.time.ntp_server = F("pool.ntp.org,time.nist.gov");
  session.time.gmt_offset = 0;
  session.time.day_light_offset = 0;

  SMTP_Message message;
  message.sender.name = "SmartTank Pro";
  message.sender.email = AUTHOR_EMAIL;
  message.addRecipient("Owner", AUTHOR_EMAIL);

  char subjectBuffer[96];
  snprintf(subjectBuffer, sizeof(subjectBuffer), "SmartTank Alert: %s", statusMsg.c_str());
  message.subject = subjectBuffer;

  char bodyBuffer[240];
  snprintf(bodyBuffer, sizeof(bodyBuffer),
           "Status: %s\r\nLevel: %.1f%%\r\nFill pump: %s\r\nRemove water: %s\r\nLast: %s",
           statusMsg.c_str(), waterLevelPercent, pumpStatus.c_str(),
           drainStatus.c_str(), lastFilledTime.c_str());

  message.text.content = bodyBuffer;
  message.text.charSet = "utf-8";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;

  smtp.callback(smtpCallback);
  MailClient.networkReconnect(true);
  if (!smtp.connect(&session)) { addLog("Email failed: SMTP connect"); return; }
  if (!MailClient.sendMail(&smtp, &message)) { addLog("Email failed: sendMail"); } 
  else { addLog("Email sent OK"); }
  smtp.closeSession();
}

/* ================= EMAIL TASK ================= */
void emailTask(void *p) {
  for (;;) {
    if (emailPending) {
      String msg = pendingMsg;
      emailPending = false;
      sendGmailAlert(msg);
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

/* ================= SENSOR OPTIMIZED (MEDIAN FILTER) ================= */
float readDistanceCm() {
  const int samples = 5;
  float readings[samples];
  int validCount = 0;

  for (int i = 0; i < samples; i++) {
    digitalWrite(TRIG, LOW); delayMicroseconds(2);
    digitalWrite(TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG, LOW);

    const unsigned long duration = pulseIn(ECHO, HIGH, echoTimeoutUs);
    if (duration == 0) continue;

    const float distanceCm = (duration * 0.0343f) / 2.0f;
    if (distanceCm >= 0.5f && distanceCm <= sensorMaxDistanceCm) {
      readings[validCount++] = distanceCm;
    }

    if (i + 1 < samples) delayMicroseconds(300);
  }

  if (validCount == 0) {
    return lastValidDistanceCm;
  }

  for (int i = 0; i < validCount - 1; i++) {
    for (int j = i + 1; j < validCount; j++) {
      if(readings[i] > readings[j]) {
        float temp = readings[i];
        readings[i] = readings[j];
        readings[j] = temp;
      }
    }
  }

  const float medianDistance = readings[validCount / 2];
  lastValidDistanceCm = medianDistance;
  return medianDistance;
}

float smoothLevelPercent(float instantPercent) {
  float delta = instantPercent - waterLevelPercent;
  const bool rising = delta >= 0.0f;

  float blend = rising ? idleRiseBlend : idleFallBlend;
  float maxStep = rising ? maxRiseStepPercent : maxFallStepPercent;

  if (pumpStatus == "ON" && rising) {
    blend = fillRiseBlend;
  }
  if (drainStatus == "ON" && !rising) {
    blend = drainFallBlend;
    maxStep = maxDrainFallStepPercent;
  }

  float step = delta * blend;
  if (step > maxStep) step = maxStep;
  if (step < -maxStep) step = -maxStep;

  float nextLevel = constrain(waterLevelPercent + step, 0.0f, 100.0f);

  if (fullLevelLatched && drainStatus == "OFF") {
    if (instantPercent >= fullReleasePercent) {
      fullReleaseCounter = 0;
      if (nextLevel < fullHoldPercent) nextLevel = fullHoldPercent;
    } else {
      fullReleaseCounter++;
      if (fullReleaseCounter < fullReleaseSamples) {
        if (nextLevel < fullHoldPercent) nextLevel = fullHoldPercent;
      } else {
        fullLevelLatched = false;
      }
    }
  }

  return nextLevel;
}

String buildMonthlyCsv() {
  MonthlyLogEntry snap[MONTHLY_LOG_SIZE];
  int snapCount;
  int snapHead;
  int snapYear;
  int snapMonth;

  taskENTER_CRITICAL(&mux);
  snapCount = monthlyLogCount;
  snapHead = monthlyLogHead;
  snapYear = activeLogYear;
  snapMonth = activeLogMonth;
  for (int i = 0; i < snapCount; i++) {
    const int idx = (snapHead - snapCount + i + MONTHLY_LOG_SIZE) % MONTHLY_LOG_SIZE;
    snap[i] = monthlyLogBuf[idx];
  }
  taskEXIT_CRITICAL(&mux);

  String csv;
  csv.reserve(120 + (snapCount * 64));
  csv += "month,timestamp,action\n";

  char monthLabel[8];
  if (snapYear >= 0 && snapMonth >= 0) {
    snprintf(monthLabel, sizeof(monthLabel), "%04d-%02d", snapYear + 1900, snapMonth + 1);
  } else {
    snprintf(monthLabel, sizeof(monthLabel), "live");
  }

  for (int i = 0; i < snapCount; i++) {
    csv += monthLabel;
    csv += ",";
    csv += snap[i].stamp;
    csv += ",\"";
    csv += snap[i].action;
    csv += "\"\n";
  }
  return csv;
}

/* ================= BUILD JSON CACHE ================= */
void updateJsonCache() {
  char historyPart[1200];
  int off = 0;
  off += snprintf(historyPart + off, sizeof(historyPart) - off, "\"history\":[");
  
  LogEntry snap[LOG_SIZE];
  int snapCount, snapHead;

  taskENTER_CRITICAL(&mux);
  snapCount = logCount;
  snapHead = logHead;
  for (int i = 0; i < snapCount; i++) {
    int idx = (snapHead - 1 - i + LOG_SIZE) % LOG_SIZE;
    snap[i] = logBuf[idx];
  }
  taskEXIT_CRITICAL(&mux);

  for (int i = 0; i < snapCount; i++) {
    off += snprintf(historyPart + off, sizeof(historyPart) - off,
                    "%s{\"ts\":\"%s\",\"action\":\"%s\"}",
                    (i == 0 ? "" : ","), snap[i].ts, snap[i].action);
    if (off >= (int)sizeof(historyPart) - 80) break;
  }
  off += snprintf(historyPart + off, sizeof(historyPart) - off, "]");

  taskENTER_CRITICAL(&mux);
  snprintf(dataJson, sizeof(dataJson),
           "{\"level\":%.1f,\"tank\":\"%s\",\"pump\":\"%s\",\"drain\":\"%s\",\"drainEnabled\":%s,\"last\":\"%s\",\"monthly\":%d,%s}",
           waterLevelPercent, tankStatus.c_str(), pumpStatus.c_str(), drainStatus.c_str(),
           drainEnabled ? "true" : "false", lastFilledTime.c_str(), monthlyFills, historyPart);
  taskEXIT_CRITICAL(&mux);
}

/* ================= UI HTML ================= */
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Tank Pro | Real-Time</title>
    <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
    <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/bootstrap-icons@1.11.0/font/bootstrap-icons.css">
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;500;700&display=swap" rel="stylesheet">
    <style>
        :root { --primary-blue: #007aff; --success-green: #34c759; --sea-blue: #00d2ff; --bg-light: #f4f7f9; }
        body { background-color: var(--bg-light); font-family: 'Outfit', sans-serif; }
        .smart-card { border: 2px solid transparent; border-radius: 28px; background: white; box-shadow: 0 10px 40px rgba(0,0,0,0.04); transition: all 0.4s ease; }
        @keyframes glow-pump { 0% { box-shadow: 0 0 5px rgba(52, 199, 89, 0.2); } 50% { box-shadow: 0 0 20px rgba(52, 199, 89, 0.4); border-color: var(--success-green); } 100% { box-shadow: 0 0 5px rgba(52, 199, 89, 0.2); } }
        @keyframes glow-filling { 0% { box-shadow: 0 0 5px rgba(0, 210, 255, 0.2); } 50% { box-shadow: 0 0 20px rgba(0, 210, 255, 0.4); border-color: var(--sea-blue); } 100% { box-shadow: 0 0 5px rgba(0, 210, 255, 0.2); } }
        .pump-active { animation: glow-pump 2s infinite ease-in-out; }
        .filling-active { animation: glow-filling 2s infinite ease-in-out; }
        .tank-visual { width: 120px; height: 180px; border: 4px solid #eee; border-radius: 20px; position: relative; margin: 0 auto; overflow: hidden; background: #fff; }
        .water-wave { position: absolute; bottom: 0; width: 100%; background: linear-gradient(180deg, #4facfe 0%, #007aff 100%); transition: height 0.4s ease-out; }
        .water-wave::before { content: ""; position: absolute; top: -15px; left: 0; width: 200%; height: 30px; background: url('https://raw.githubusercontent.com/front-end-relative/water-wave-animation/main/wave.png'); background-size: 50% 30px; animation: move-wave 2s linear infinite; opacity: 0.5; }
        @keyframes move-wave { 0% { transform: translateX(0); } 100% { transform: translateX(-50%); } }
        .bi-spin { display: inline-block; animation: spin 2s linear infinite; }
        @keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }
        .status-dot { height: 10px; width: 10px; border-radius: 50%; display: inline-block; }
        .blink { animation: blinker 1s linear infinite; }
        @keyframes blinker { 50% { opacity: 0; } }
        .v-btn { border-radius: 50px; font-size: 0.7rem; padding: 2px 10px; }
        .drain-btn { border-radius: 50px; font-size: 0.75rem; padding: 6px 14px; min-width: 110px; }
        .download-btn { border-radius: 50px; font-size: 0.75rem; padding: 6px 14px; text-decoration: none; }
        .log-container { border: 1px solid #eee; border-radius: 15px; overflow-y: auto; max-height: 300px; }
        .table thead th { position: sticky; top: 0; background: #f8f9fa; z-index: 10; border-bottom: 2px solid #eee; }
    </style>
</head>
<body>
<div class="container py-4">
    <div class="row mb-4 align-items-center">
        <div class="col-6">
            <h2 class="fw-bold mb-0">SmartTank <span class="text-primary">Pro</span></h2>
            <small><span id="conn-dot" class="status-dot bg-danger blink"></span> <span id="conn-text">Syncing...</span></small>
            <button id="v-btn" class="btn btn-outline-primary v-btn ms-2" onclick="toggleVoice()">ENABLE VOICE</button>
        </div>
        <div class="col-6 text-end"><h5 id="clock" class="fw-bold mb-0">--:--:--</h5></div>
    </div>
    <div class="row g-3">
        <div class="col-md-4">
            <div class="smart-card p-4 text-center">
                <div class="tank-visual mb-3"><div class="water-wave" id="wave-fill"></div></div>
                <h2 class="fw-bold mb-0"><span id="level">0</span>%</h2>
                <small class="text-muted fw-bold">LIVE CAPACITY</small>
            </div>
        </div>
        <div class="col-md-8">
            <div class="row g-3">
                <div class="col-6"><div id="status-card" class="smart-card p-3 d-flex align-items-center h-100">
                    <i class="bi bi-activity text-primary fs-3 me-3"></i>
                    <div><small class="text-muted fw-bold">STATUS</small><div class="fw-bold h5 mb-0" id="tank">--</div></div>
                </div></div>
                <div class="col-6"><div id="pump-card" class="smart-card p-3 d-flex align-items-center h-100">
                    <i id="pump-icon" class="bi bi-fan fs-3 me-3"></i>
                    <div><small class="text-muted fw-bold">PUMP</small><div class="fw-bold h5 mb-0" id="pump">--</div></div>
                </div></div>
                <div class="col-12"><div id="drain-card" class="smart-card p-3 d-flex justify-content-between align-items-center">
                    <div class="d-flex align-items-center">
                        <i id="drain-icon" class="bi bi-droplet-half text-info fs-3 me-3"></i>
                        <div><small class="text-muted fw-bold">REMOVE WATER</small><div class="fw-bold h5 mb-0"><span id="drain">--</span> <span id="drain-mode" class="badge bg-light text-dark border ms-1">OFF</span></div></div>
                    </div>
                    <button id="drain-btn" class="btn btn-outline-info drain-btn" onclick="toggleDrain()">ENABLE</button>
                </div></div>
                <div class="col-12"><div class="smart-card p-3 d-flex justify-content-between align-items-center">
                    <div><small class="text-muted fw-bold">MONTHLY TOTAL</small><div class="h3 fw-bold mb-0" id="monthly">0</div></div>
                    <div class="text-end"><small class="text-muted">Last Activity</small><div id="last" class="fw-bold">--</div></div>
                </div></div>
            </div>
        </div>
    </div>
    <div class="smart-card mt-4 p-4">
        <div class="d-flex justify-content-between align-items-center mb-3">
            <h6 class="fw-bold mb-0">Live Hardware Log</h6>
            <div class="d-flex align-items-center gap-2">
                <span class="badge bg-light text-dark border">120ms refresh</span>
                <a href="/monthly-log.csv" class="btn btn-outline-dark download-btn">DOWNLOAD MONTH LOG</a>
            </div>
        </div>
        <div class="log-container">
            <div class="table-responsive"><table class="table table-sm align-middle mb-0">
                <thead><tr class="text-muted small"><th class="ps-3">TIMESTAMP</th><th>ACTION</th></tr></thead>
                <tbody id="history-body"></tbody>
            </table></div>
        </div>
    </div>
</div>
<script>
    const REFRESH_MS = 120;
    let lastS = ""; let voiceEnabled = false;
    function setVoiceButtonUI() {
        const b = document.getElementById('v-btn');
        b.innerText = voiceEnabled ? "DISABLE VOICE" : "ENABLE VOICE";
        b.className = voiceEnabled ? "btn btn-primary v-btn ms-2" : "btn btn-outline-primary v-btn ms-2";
    }
    function speak(t) {
        if (!voiceEnabled) return;
        window.speechSynthesis.cancel();
        window.speechSynthesis.speak(new SpeechSynthesisUtterance(t));
    }
    function toggleVoice() { voiceEnabled = !voiceEnabled; if(voiceEnabled) speak("Voice enabled."); setVoiceButtonUI(); }
    async function toggleDrain() {
        const btn = document.getElementById("drain-btn");
        const enable = btn.dataset.enabled !== "true";
        btn.disabled = true;
        try { await fetch("/drain?enable=" + (enable ? "1" : "0")); }
        finally { btn.disabled = false; update(); }
    }
    setInterval(() => { document.getElementById('clock').innerHTML = new Date().toLocaleTimeString(); }, 1000);
    function renderHistory(arr) {
        const body = document.getElementById("history-body");
        body.innerHTML = "";
        arr.forEach(item => {
            const tr = `<tr><td class="text-muted small ps-3">${item.ts}</td><td>${item.action}</td></tr>`;
            body.innerHTML += tr;
        });
    }
    async function update() {
        try {
            const r = await fetch("/data", { cache: "no-store" });
            const d = await r.json();
            document.getElementById("level").innerText = d.level.toFixed(1);
            document.getElementById("tank").innerText = d.tank;
            document.getElementById("pump").innerText = d.pump;
            document.getElementById("drain").innerText = d.drain;
            document.getElementById("drain-mode").innerText = d.drainEnabled ? "AUTO" : "OFF";
            document.getElementById("last").innerText = d.last;
            document.getElementById("monthly").innerText = d.monthly;
            document.getElementById('wave-fill').style.height = d.level + '%';
            const pCard = document.getElementById("pump-card");
            const pIcon = document.getElementById("pump-icon");
            if (d.pump === "ON") { pCard.classList.add("pump-active"); pIcon.classList.add("bi-spin"); }
            else { pCard.classList.remove("pump-active"); pIcon.classList.remove("bi-spin"); }
            const sCard = document.getElementById("status-card");
            if (d.tank === "FILLING") sCard.classList.add("filling-active");
            else sCard.classList.remove("filling-active");
            const dCard = document.getElementById("drain-card");
            const dIcon = document.getElementById("drain-icon");
            const dBtn = document.getElementById("drain-btn");
            dBtn.dataset.enabled = d.drainEnabled ? "true" : "false";
            dBtn.innerText = d.drainEnabled ? "DISABLE" : "ENABLE";
            dBtn.className = d.drainEnabled ? "btn btn-info text-white drain-btn" : "btn btn-outline-info drain-btn";
            if (d.drain === "ON") { dCard.classList.add("filling-active"); dIcon.classList.add("bi-spin"); }
            else { dCard.classList.remove("filling-active"); dIcon.classList.remove("bi-spin"); }
            if (d.tank !== lastS) {
                if (d.tank === "FULL") speak("Water tank is full.");
                else if (d.tank === "FILLING") speak("Pump on. Filling tank.");
                else if (d.tank === "DRAINING") speak("Removing water.");
                lastS = d.tank;
            }
            renderHistory(d.history);
            document.getElementById("conn-dot").className = "status-dot bg-success";
            document.getElementById("conn-text").innerText = "Live";
        } catch (e) {
            document.getElementById("conn-dot").className = "status-dot bg-danger blink";
            document.getElementById("conn-text").innerText = "Reconnecting...";
        }
    }
    async function poll() {
        await update();
        setTimeout(poll, REFRESH_MS);
    }
    poll();
</script>
</body>
</html>
)rawliteral";

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(RELAY, OUTPUT); pinMode(DRAIN_RELAY, OUTPUT); pinMode(BUZZER, OUTPUT);
  digitalWrite(RELAY, HIGH); digitalWrite(DRAIN_RELAY, HIGH); digitalWrite(BUZZER, LOW);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  timeClient.begin();
  timeClient.update();
  syncMonthlyWindow();
  xTaskCreatePinnedToCore(emailTask, "emailTask", 8192, NULL, 1, NULL, 0);

  addLog("System boot");
  updateJsonCache();
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    String payload;
    taskENTER_CRITICAL(&mux);
    payload = dataJson;
    taskEXIT_CRITICAL(&mux);
    AsyncWebServerResponse *response = request->beginResponse(200, "application/json", payload);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });
  server.on("/monthly-log.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    const String csv = buildMonthlyCsv();
    AsyncWebServerResponse *response = request->beginResponse(200, "text/csv", csv);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Content-Disposition", "attachment; filename=monthly_log.csv");
    request->send(response);
  });
  server.on("/drain", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool enable = request->hasParam("enable") && request->getParam("enable")->value() == "1";
    drainEnabled = enable;

    if (drainEnabled) {
      drainCycleArmed = waterLevelPercent >= fillStopPercent;
      addLog(drainCycleArmed ? "Drain auto enabled" : "Drain armed, waiting FULL");
    } else {
      drainCycleArmed = false;
      if (drainStatus == "ON") {
        digitalWrite(DRAIN_RELAY, HIGH);
        drainStatus = "OFF";
        fullReleaseCounter = 0;
        addLog("Drain pump OFF (web)");
      } else {
        addLog("Drain auto disabled");
      }
    }

    updateJsonCache();
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.begin();
}

/* ================= LOOP ================= */
void loop() {
  timeClient.update();
  if (syncMonthlyWindow()) {
    lastFilledTime = "--:--:--";
    fullLevelLatched = false;
    fullReleaseCounter = 0;
    updateJsonCache();
  }

  if (millis() - lastMeasureTime >= measureInterval) {
    lastMeasureTime = millis();

    float distance = readDistanceCm();
    float waterHeight = tankHeight - distance;
    float instantPercent = constrain((waterHeight / tankHeight) * 100.0f, 0.0f, 100.0f);

    // Apply asymmetric smoothing so the tank rises and falls more naturally.
    waterLevelPercent = smoothLevelPercent(instantPercent);

    if (drainEnabled && waterLevelPercent >= fillStopPercent) drainCycleArmed = true;

    if (drainEnabled && drainCycleArmed && drainStatus == "OFF" && pumpStatus == "OFF" && waterLevelPercent >= fillStopPercent) {
      digitalWrite(RELAY, HIGH);
      pumpStatus = "OFF";
      digitalWrite(DRAIN_RELAY, LOW);
      drainStatus = "ON";
      fullLevelLatched = false;
      fullReleaseCounter = 0;
      tankStatus = "DRAINING";
      addLog("Drain pump ON (full)");
    }
    else if (drainStatus == "ON" && (!drainEnabled || waterLevelPercent <= drainStopPercent)) {
      digitalWrite(DRAIN_RELAY, HIGH);
      drainStatus = "OFF";
      drainCycleArmed = false;
      tankStatus = "NORMAL";
      addLog("Drain pump OFF (safe level)");
      fullReleaseCounter = 0;
    }

    if (drainStatus == "ON") {
      tankStatus = "DRAINING";
    }
    else if (pumpStatus == "ON" && waterLevelPercent < fillStopPercent) tankStatus = "FILLING";
    else if (fullLevelLatched && pumpStatus == "OFF" && drainStatus == "OFF") tankStatus = "FULL";

    if (drainStatus == "OFF" && waterLevelPercent <= fillStartPercent && pumpStatus == "OFF") {
      digitalWrite(RELAY, LOW);
      pumpStatus = "ON"; tankStatus = "FILLING";
      alreadyCounted = false;
      fullLevelLatched = false;
      fullReleaseCounter = 0;
      addLog("Pump ON (low level)");
    }
    else if (waterLevelPercent >= fillStopPercent && pumpStatus == "ON") {
      digitalWrite(RELAY, HIGH);
      pumpStatus = "OFF"; tankStatus = "FULL";
      if (!alreadyCounted) {
        lastFilledTime = timeClient.getFormattedTime();
        monthlyFills++; alreadyCounted = true;
        fullLevelLatched = true;
        fullReleaseCounter = 0;
        if (waterLevelPercent < fullHoldPercent) waterLevelPercent = fullHoldPercent;
        digitalWrite(BUZZER, HIGH); delay(120); digitalWrite(BUZZER, LOW);
        addLog("Tank FULL, pump OFF");
        queueEmailAlert("TANK FULL");
      }
    }
    else if (pumpStatus == "OFF" && drainStatus == "OFF" && waterLevelPercent > fillStartPercent && waterLevelPercent < fillStopPercent) {
      tankStatus = "NORMAL";
    }

    updateJsonCache();
  }
}
