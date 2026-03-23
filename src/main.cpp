#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Arduino.h>
#include "BluetoothSerial.h"
#include <WiFi.h>
#include <WebServer.h>

void initOBD2();
String sendOBD(String command);
void webServerTask(void * pvParameters);

const int RELAY_PIN = 4;
const int SENSOR_UNLOCK_PIN = 18;
const char* ssid = "Iqi";
const char* password = "apaweahkepo";

BluetoothSerial SerialBT;
WebServer server(80);

volatile int currentSpeed = 0, currentRPM = 0, currentTemp = 0;
volatile float currentVolt = 0.0, currentMAF = 0.0;
volatile int currentIAT = 0, currentTiming = 0, currentThrottle = 0;
volatile float currentSTFT = 0.0, currentLTFT = 0.0;
volatile bool isLocked = false, obdConnected = false, manualDisconnect = false;
volatile bool isScanningDTC = false;
bool otaInitialized = false;
bool ipReported = false;

unsigned long lastReconnectAttempt = 0;
unsigned long lastValidDataTime = 0; // Watchdog timer
int reconnectFailCount = 0;

unsigned long lastWiFiCheck = 0;

unsigned long lastUnlockTime = 0;
unsigned long relayTriggerTime = 0;
bool isRelayActive = false;

const int POLL_SEQ[]   = {0, 9, 0, 1, 0, 8, 0, 9, 0, 1, 3, 0, 9, 0, 1, 0, 6, 0, 9, 0, 1, 2, 4, 5, 7};
const int POLL_SEQ_LEN = 25;
int seqIdx = 0;

uint8_t elmMacAddress[6] = {0x00, 0x1D, 0xA5, 0x06, 0x0F, 0x38};

TaskHandle_t WebTask;

String webLog = "";
void addLog(String message) {
  String logEntry = "[" + String(millis()) + " ms] " + message + "<br>";
  webLog += logEntry;
  if (webLog.length() > 5000) {
    webLog = "";
    webLog += "<b style='color:#f44'>[SYSTEM] Log dibersihkan otomatis.</b><br>";
  }
}

void initOBD2() {
  addLog("<b style='color:#0ff'>=== INIT NISSAN K-LINE ===</b>");
  sendOBD("ATZ");           delay(500);
  sendOBD("ATE0");          delay(200);
  sendOBD("ATSP5");         delay(200);
  sendOBD("ATSH 81 10 FC"); delay(200);
  sendOBD("ATAT1");         delay(100);
  sendOBD("ATST 10");       delay(100);
  sendOBD("ATFI");          delay(1500);
  addLog("<b style='color:#0ff'>=== ECU BANGUN ===</b>");
}

String sendOBD(String command) {
  while (SerialBT.available()) SerialBT.read();
  SerialBT.print(command + "\r");
  String response = "";
  response.reserve(50); 
  
  // Mencegah bug millis overflow 50 hari
  unsigned long startTimer = millis(); 
  while (millis() - startTimer < 800) {
    if (SerialBT.available()) {
      char c = SerialBT.read();
      if (c == '>') break; 
      response += c;
    }
    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
  
  response.replace("\r", "");
  response.replace("\n", "");
  response.replace(" ", "");
  addLog("Kirim: " + command + " | Balasan: " + response);
  return response;
}

void webServerTask(void * pvParameters) {
  for(;;) {
    server.handleClient();
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setupOTA() {
  ArduinoOTA.setHostname("Livina-ESP32");
  ArduinoOTA.setPassword("12345678");

  ArduinoOTA.onStart([](){ Serial.println("OTA Update Dimulai..."); });
  ArduinoOTA.onEnd([](){ Serial.println("\nOTA Selesai!"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total){
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error){
    Serial.printf("OTA Error[%u]\n", error);
  });

  ArduinoOTA.begin();
  otaInitialized = true;
  Serial.println("OTA READY! Siap menerima update tanpa kabel.");
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(SENSOR_UNLOCK_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, password);

  Serial.print("Menghubungkan ke WiFi");
 
  server.on("/log", []() {
    String html = R"rawliteral(<!DOCTYPE html><html><head><meta charset="UTF-8"><title>OBD Log Monitor</title><style>body { background:#0a0a0a; color:#00ff88; font-family:monospace; font-size:13px; margin:0; padding:0; } #bar { position:fixed; top:0; left:0; right:0; background:#111; padding:8px 12px; border-bottom:1px solid #333; display:flex; align-items:center; gap:10px; z-index:99; } #bar h3 { margin:0; color:#0ff; font-size:14px; } .btn { padding:4px 12px; border:none; border-radius:4px; cursor:pointer; font-size:12px; font-weight:bold; } #btnPause { background:#444; color:#fff; } #btnClear { background:#a00; color:#fff; } #counter { color:#888; font-size:11px; margin-left:auto; } #log { margin-top:42px; padding:10px 14px; line-height:1.6; }</style></head><body><div id="bar"><h3>&#9654; OBD LIVE LOG</h3><button class="btn" id="btnPause" onclick="togglePause()">&#9646;&#9646; Pause</button><button class="btn" id="btnClear" onclick="clearLog()">&#128465; Clear</button><span id="counter">0 baris</span></div><div id="log"></div><script>let paused = false; let lineCount = 0; let lastLen = 0; function togglePause() { paused = !paused; document.getElementById('btnPause').innerHTML = paused ? '&#9654; Resume' : '&#9646;&#9646; Pause'; } function clearLog() { fetch('/log_clear').then(() => { document.getElementById('log').innerHTML = ''; lineCount = 0; lastLen = 0; }); } function pollLog() { if (paused) { setTimeout(pollLog, 300); return; } fetch('/log_data?from=' + lastLen).then(r => r.json()).then(d => { if (d.reset) { document.getElementById('log').innerHTML = ''; lineCount = 0; lastLen = 0; return; } if (d.lines && d.lines.length > 0) { const el = document.getElementById('log'); d.lines.forEach(line => { const div = document.createElement('div'); div.innerHTML = line; el.appendChild(div); lineCount++; }); lastLen = d.total; document.getElementById('counter').textContent = lineCount + ' baris'; window.scrollTo(0, document.body.scrollHeight); } setTimeout(pollLog, 150); }).catch(() => setTimeout(pollLog, 500)); } pollLog();</script></body></html>)rawliteral";
    server.send(200, "text/html", html);
  });

  server.on("/log_data", []() {
    int fromIdx = 0;
    if (server.hasArg("from")) fromIdx = server.arg("from").toInt();
    
    if (fromIdx > 0 && webLog.length() < 100) {
       server.send(200, "application/json", "{\"reset\":true}");
       return;
    }
    
    String json = "{\"total\":0,\"lines\":[";
    int total = 0, cur = 0, start = 0;
    bool first = true;
    int wlen = webLog.length();
    
    for (int i = 0; i <= wlen - 3; i++) {
      if (webLog.substring(i, i+4) == "<br>") {
        if (cur >= fromIdx) {
          String line = webLog.substring(start, i);
          line.replace("\"", "\\\"");
          if (!first) json += ",";
          json += "\"" + line + "\"";
          first = false;
        }
        total++; cur++; start = i + 4;
      }
    }
    json += "],\"total\":" + String(total) + "}";
    server.send(200, "application/json", json);
  });

  server.on("/log_clear", []() { webLog = ""; server.send(200, "text/plain", "OK"); });
  server.on("/obd_connect", []() { manualDisconnect = false; lastReconnectAttempt = 0; server.send(200, "text/plain", "OK"); });
  server.on("/obd_disconnect", []() { manualDisconnect = true; SerialBT.disconnect(); obdConnected = false; server.send(200, "text/plain", "OK"); });

  server.on("/data", []() {
    String json = "{\"speed\":" + String(currentSpeed) + ",\"rpm\":" + String(currentRPM) +
                  ",\"temp\":" + String(currentTemp) + ",\"volt\":" + String(currentVolt, 1) +
                  ",\"maf\":" + String(currentMAF, 2) + ",\"iat\":" + String(currentIAT) +
                  ",\"timing\":" + String(currentTiming) + ",\"stft\":" + String(currentSTFT , 1) +
                  ",\"ltft\":" + String(currentLTFT) + ",\"throttle\":" + String(currentThrottle) +
                  ",\"locked\":" + String(isLocked ? "true" : "false") + 
                  ",\"obd_connected\":" + String(obdConnected ? "true" : "false") + 
                  ",\"manual_disconnect\":" + String(manualDisconnect ? "true" : "false") + "}";
    server.send(200, "application/json", json);
  });

    server.on("/scan_dtc", [](){
      isScanningDTC = true;
      vTaskDelay(300 / portTICK_PERIOD_MS);

      String res = sendOBD("18020000");

      String json = "{\"raw_dtc\":\"" + res + "\"}";
      server.send(200, "application/json", json);

      isScanningDTC = false;
    });

    server.on("/clear_dtc", [](){
      isScanningDTC = true;
      vTaskDelay(300 / portTICK_PERIOD_MS);

      String res = sendOBD("140000");

      server.send(200, "application/json", "{\"status\":\"cleared\", \"raw\":\"" + res + "\"}");
      
      isScanningDTC = false;
    });
  
  server.begin();
  xTaskCreatePinnedToCore(webServerTask, "WebTask", 4096, NULL, 1, &WebTask, 0);
  SerialBT.begin("ESP32_Livina", true); 
}

void loop() {
  
  if (millis() - lastWiFiCheck > 5000){
    lastWiFiCheck = millis ();
    if(WiFi.status() == WL_CONNECTED && !otaInitialized){
      setupOTA();
    }else if(WiFi.status() != WL_CONNECTED){
      otaInitialized = false;
    }
  }
  
  if (otaInitialized){
    ArduinoOTA.handle();
  }

  if (WiFi.status() == WL_CONNECTED && !ipReported) {
    HTTPClient http;
    http.begin("http://dweet.io/dweet/for/livinaprodash_iqi?ip=" + WiFi.localIP().toString());
    http.GET();
    http.end();
    ipReported = true;
    addLog("IP Terlacak & Dilaporkan: " + WiFi.localIP().toString());
  } else if (WiFi.status() != WL_CONNECTED) {
    ipReported = false; // Reset jika wifi putus
  }

  if (manualDisconnect) { vTaskDelay(100 / portTICK_PERIOD_MS); return; }
  if (isScanningDTC){ vTaskDelay(100 / portTICK_PERIOD_MS); return; }

  if (!SerialBT.hasClient()) {
    if (obdConnected) {
      obdConnected = false; isLocked = false; reconnectFailCount = 0;
      addLog("<b style='color:#fa0'>[BT] Koneksi ke ELM327 terputus.</b>");
    }

    unsigned long interval = min(5000UL + (unsigned long)reconnectFailCount * 2000UL, 15000UL);

    if (millis() - lastReconnectAttempt > interval) {
      lastReconnectAttempt = millis();
      reconnectFailCount++;
      addLog("[BT] Mencoba reconnect... (ke-" + String(reconnectFailCount) + ")");

      SerialBT.disconnect(); // Pastikan memory socket BT bersih
      vTaskDelay(200 / portTICK_PERIOD_MS);

      bool isWifiConnected = (WiFi.status() == WL_CONNECTED);
      if (!isWifiConnected) {
        WiFi.mode(WIFI_OFF); // Matikan radio WiFi 100%
        vTaskDelay(150 / portTICK_PERIOD_MS);
      }

      bool isBtConnected = SerialBT.connect(elmMacAddress);

      if (!isWifiConnected) {
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, password);
      }
      // ====================================================

      if (isBtConnected) {
        vTaskDelay(500 / portTICK_PERIOD_MS);
        SerialBT.print("\r"); // Wake-up knock untuk ELM327 Sleep Mode
        vTaskDelay(500 / portTICK_PERIOD_MS);
        
        while (SerialBT.available()) SerialBT.read();

        reconnectFailCount = 0;
        initOBD2();
        obdConnected = true;
        lastValidDataTime = millis(); // Reset watchdog
        addLog("<b style='color:#4f4'>[BT] Reconnect berhasil!</b>");
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
    return;
  }

  if (!obdConnected) reconnectFailCount = 0;
  obdConnected = true;

  // Watchdog: Cegah Zombie Connection
  if (millis() - lastValidDataTime > 8000) { 
    addLog("<b style='color:#f00'>[SYSTEM] OBD Zombie. Restart koneksi...</b>");
    SerialBT.disconnect();
    obdConnected = false;
    vTaskDelay(500 / portTICK_PERIOD_MS);
    return;
  }

  int pid = POLL_SEQ[seqIdx];
  seqIdx = (seqIdx + 1) % POLL_SEQ_LEN;

  if (pid == 0) { // RPM
    String res = sendOBD("2212010401"); 
    int idx = res.indexOf("621201");
    if (idx != -1 && res.length() >= idx + 10) {
      int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      int B = strtol(res.substring(idx + 8, idx + 10).c_str(), NULL, 16);
      currentRPM = (int)((A * 3200.0) + (B * 12.5));
      lastValidDataTime = millis(); 
    }
  }
  else if (pid == 1) { // SPEED
    String res = sendOBD("2211020401");
    int idx = res.indexOf("621102");
    if (idx != -1 && res.length() >= idx + 8) {
      currentSpeed = (int)(strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16) * 2.0);
      lastValidDataTime = millis(); 
      if (currentSpeed >= 15 && !isLocked && !isRelayActive) {
        digitalWrite(RELAY_PIN, LOW); 
        relayTriggerTime = millis();  
        isRelayActive = true;         
        isLocked = true;
        addLog("Auto-Lock Aktif");
      }
    }
  }
  else if (pid == 2) { // TEMP
    String res = sendOBD("2211010401");
    int idx = res.indexOf("621101");
    if (idx != -1 && res.length() >= idx + 8) {
      currentTemp = (int)strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16) - 50;
    }
  }
  else if (pid == 3) { // MAF
    String res = sendOBD("2212090401");
    int idx = res.indexOf("621209");
    if (idx != -1 && res.length() >= idx + 10) {
      float valA = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      float valB = strtol(res.substring(idx + 8, idx + 10).c_str(), NULL, 16);
      currentMAF = (valA * 1.28) + ((valB * 1.27) / 255.0);
    }
  }
  else if (pid == 4) { // VOLT
    String res = sendOBD("ATRV");
    if (res.indexOf("V") != -1) { res.replace("V", ""); currentVolt = res.toFloat(); }
  }
  else if (pid == 5) { // IAT (Kandidat Suhu Nissan: 1106)
    String res = sendOBD("2211060401"); 
    int idx = res.indexOf("621106");
    if (idx != -1 && res.length() >= idx + 8) {
      int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      currentIAT = A - 50; 
    }
  }
  else if (pid == 6) { // STFT (Nissan A/F Alpha B1: 1123)
    String res = sendOBD("2211230401");
    int idx = res.indexOf("621123");
    if (idx != -1 && res.length() >= idx + 8) {
      int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      // Di Nissan, angka 100 berarti 0% (Campuran Ideal)
      currentSTFT = A - 100.0; 
    }
  }
  else if (pid == 7) { // LTFT (Nissan A/F Alpha Self Learn: 1125)
    String res = sendOBD("2211250401");
    int idx = res.indexOf("621125");
    if (idx != -1 && res.length() >= idx + 8) {
      int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      currentLTFT = A - 100.0;
    }
  }
  else if (pid == 8) { // TIMING (Nissan 22110A)
    String res = sendOBD("22110A0401");
    int idx = res.indexOf("62110A");
    if (idx != -1 && res.length() >= idx + 8) {
      int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      currentTiming = 110 - A; // Rumus dari file Java
    }
  }
  else if (pid == 9) { // THROTTLE (Nissan 221117)
    String res = sendOBD("2211170401");
    int idx = res.indexOf("621117");
    if (idx != -1 && res.length() >= idx + 8) {
      int A = strtol(res.substring(idx + 6, idx + 8).c_str(), NULL, 16);
      currentThrottle = (A * 100) / 256; 
    }
  }

  if (isRelayActive && (millis() - relayTriggerTime >= 500)) {
    digitalWrite(RELAY_PIN, HIGH);
    isRelayActive = false;
  }

  // 2. Sensor Unlock Penumpang (Non-Blocking Debounce 1 Detik)
  if (digitalRead(SENSOR_UNLOCK_PIN) == LOW) {
    if (millis() - lastUnlockTime > 1000) { 
      if (isLocked) {
        isLocked = false; // Reset memori agar bisa auto-lock lagi pas jalan
        addLog("Tombol Unlock Ditekan! Siap mengunci kembali di 20 km/h.");
      }
      lastUnlockTime = millis();
    }
  }

  vTaskDelay(10 / portTICK_PERIOD_MS); 
}