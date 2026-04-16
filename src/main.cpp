#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ESP_Mail_Client.h>

/* ================= WIFI & GMAIL ================= */
const char *ssid = "realme C12i";
const char *password = "Germin@2023";

#define SMTP_HOST       "smtp.gmail.com"
#define SMTP_PORT       465
#define AUTHOR_EMAIL    "gkisheo@gmail.com"
#define APP_PASSWORD    "imwx dpqu jtbt mhte"
#define RECIPIENT_EMAIL "karlpeter491@gmail.com"

/* ================= PINS ================= */
#define TRIG 5
#define ECHO 18
#define RELAY 17
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
String lastFilledTime = "--:--:--";
int monthlyFills = 0;
bool alreadyCounted = false;

unsigned long lastMeasureTime = 0;
const unsigned long measureInterval = 100; // Reduced to 100ms for faster "Real-Time" feel

/* ================= FAST JSON CACHE ================= */
char dataJson[1800];
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

/* ================= EMAIL QUEUE (NON-BLOCKING) ================= */
volatile bool emailPending = false;
String pendingMsg = "";

/* ================= LIVE HARDWARE LOG ================= */
static const int LOG_SIZE = 20;

struct LogEntry {
  char ts[16];
  char action[40];
};

LogEntry logBuf[LOG_SIZE];
volatile int logHead = 0;
volatile int logCount = 0;

void addLog(const char *action) {
  String t = timeClient.getFormattedTime();
  char ts[16];
  snprintf(ts, sizeof(ts), "%s", t.c_str());

  taskENTER_CRITICAL(&mux);
  snprintf(logBuf[logHead].ts, sizeof(logBuf[logHead].ts), "%s", ts);
  snprintf(logBuf[logHead].action, sizeof(logBuf[logHead].action), "%s", action);
  logHead = (logHead + 1) % LOG_SIZE;
  if (logCount < LOG_SIZE) logCount++;
  taskEXIT_CRITICAL(&mux);
}

/* ================= SMTP CALLBACK ================= */
void smtpCallback(SMTP_Status status) {
  Serial.println(status.info());
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

  SMTP_Message message;
  message.sender.name = "SmartTank Pro";
  message.sender.email = AUTHOR_EMAIL;
  message.addRecipient("User", RECIPIENT_EMAIL);

  char subjectBuffer[96];
  snprintf(subjectBuffer, sizeof(subjectBuffer), "SmartTank Alert: %s", statusMsg.c_str());
  message.subject = subjectBuffer;

  char bodyBuffer[240];
  snprintf(bodyBuffer, sizeof(bodyBuffer),
           "Status: %s\r\nLevel: %.1f%%\r\nPump: %s\r\nLast: %s",
           statusMsg.c_str(), waterLevelPercent, pumpStatus.c_str(), lastFilledTime.c_str());

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
  const int SAMPLES = 5;
  float readings[SAMPLES];
  
  for(int i=0; i<SAMPLES; i++) {
    digitalWrite(TRIG, LOW); delayMicroseconds(2);
    digitalWrite(TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG, LOW);
    // Use interrupts-disabled for microsecond timing accuracy
    unsigned long duration = pulseIn(ECHO, HIGH, 20000UL); 
    readings[i] = (duration == 0) ? 999.0f : (duration * 0.0343f) / 2.0f;
    delay(10); // Short gap between samples
  }

  // Simple Bubble Sort to find Median (removes 10% -> 50% spikes)
  for(int i=0; i<SAMPLES-1; i++) {
    for(int j=i+1; j<SAMPLES; j++) {
      if(readings[i] > readings[j]) {
        float temp = readings[i];
        readings[i] = readings[j];
        readings[j] = temp;
      }
    }
  }
  return readings[SAMPLES/2]; // Return middle value
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
           "{\"level\":%.1f,\"tank\":\"%s\",\"pump\":\"%s\",\"last\":\"%s\",\"monthly\":%d,%s}",
           waterLevelPercent, tankStatus.c_str(), pumpStatus.c_str(), 
           lastFilledTime.c_str(), monthlyFills, historyPart);
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
            <span class="badge bg-light text-dark border">Auto-refreshing</span>
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
    setInterval(() => { document.getElementById('clock').innerHTML = new Date().toLocaleTimeString(); }, 1000);
    function renderHistory(arr) {
        const body = document.getElementById("history-body");
        body.innerHTML = "";
        arr.forEach(item => {
            const tr = <tr><td class="text-muted small ps-3">${item.ts}</td><td>${item.action}</td></tr>;
            body.innerHTML += tr;
        });
    }
    async function update() {
        try {
            const r = await fetch("/data");
            const d = await r.json();
            document.getElementById("level").innerText = d.level.toFixed(1);
            document.getElementById("tank").innerText = d.tank;
            document.getElementById("pump").innerText = d.pump;
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
            if (d.tank !== lastS) {
                if (d.tank === "FULL") speak("Water tank is full.");
                else if (d.tank === "FILLING") speak("Pump on. Filling tank.");
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
    setInterval(update, 250);
</script>
</body>
</html>
)rawliteral";

/* ================= SETUP ================= */
void setup() {
  Serial.begin(115200);
  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT);
  pinMode(RELAY, OUTPUT); pinMode(BUZZER, OUTPUT);
  digitalWrite(RELAY, HIGH); digitalWrite(BUZZER, LOW);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  timeClient.begin();
  xTaskCreatePinnedToCore(emailTask, "emailTask", 8192, NULL, 1, NULL, 0);

  addLog("System boot");
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", index_html);
  });
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    taskENTER_CRITICAL(&mux);
    request->send(200, "application/json", dataJson);
    taskEXIT_CRITICAL(&mux);
  });
  server.begin();
}

/* ================= LOOP ================= */
void loop() {
  timeClient.update();

  if (millis() - lastMeasureTime >= measureInterval) {
    lastMeasureTime = millis();

    float distance = readDistanceCm();
    float waterHeight = tankHeight - distance;
    float instantPercent = constrain((waterHeight / tankHeight) * 100.0f, 0.0f, 100.0f);

    // EMA Smoothing Filter: 70% old value, 30% new value
    // This stops the flickering/jumps on the dashboard
    waterLevelPercent = (waterLevelPercent * 0.7f) + (instantPercent * 0.3f);

    if (pumpStatus == "ON" && waterLevelPercent < 80.0f) tankStatus = "FILLING";

    if (waterLevelPercent <= 20.0f && pumpStatus == "OFF") {
      digitalWrite(RELAY, LOW);
      pumpStatus = "ON"; tankStatus = "FILLING";
      alreadyCounted = false;
      addLog("Pump ON (low level)");
      pendingMsg = "FILLING STARTED"; emailPending = true;
    }
    else if (waterLevelPercent >= 80.0f && pumpStatus == "ON") {
      digitalWrite(RELAY, HIGH);
      pumpStatus = "OFF"; tankStatus = "FULL";
      if (!alreadyCounted) {
        lastFilledTime = timeClient.getFormattedTime();
        monthlyFills++; alreadyCounted = true;
        digitalWrite(BUZZER, HIGH); delay(120); digitalWrite(BUZZER, LOW);
        addLog("Tank FULL, pump OFF");
        pendingMsg = "TANK FULL"; emailPending = true;
      }
    }
    else if (pumpStatus == "OFF" && waterLevelPercent > 20.0f && waterLevelPercent < 80.0f) {
      tankStatus = "NORMAL";
    }

    updateJsonCache();
  }
}