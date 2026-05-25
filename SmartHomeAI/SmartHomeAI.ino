/*
 * ================================================================
 *   SmartHomeAI — ESP32 Cloud-Connected Controller
 * ================================================================
 *
 *  REQUIRED LIBRARIES (Arduino Library Manager):
 *    1. WebSockets  by Markus Sattler  (search: "WebSockets")
 *    2. ArduinoJson by Benoit Blanchon
 *
 *  WIRING:
 *    ESP32 GPIO25 → Relay IN1 (Bulb)
 *    ESP32 GPIO26 → Relay IN2 (Fan)
 *    ESP32 GPIO27 → Relay IN3 (Bed Light)
 *    ESP32 GPIO14 → Relay IN4 (Charging)
 *    ESP32 GND    → Relay GND
 *    ESP32 3.3V   → Relay VCC
 *
 *  FIRST BOOT / WIFI SETUP:
 *    1. ESP32 creates hotspot "SmartHomeAI-Setup"
 *    2. Connect your phone to that hotspot
 *    3. A setup page opens — pick your WiFi, enter password
 *    4. ESP32 saves credentials and restarts automatically
 *
 *  Serial Monitor: 115200 baud
 * ================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>

// ================================================================
//  CONFIGURATION
// ================================================================

#define SERVER_HOST   "premhomeee.duckdns.org"
#define SERVER_PORT   443
#define USE_SSL       true
#define ROOM_ID       "HOME001"

#define AP_SSID       "SmartHomeAI-Setup"   // hotspot name during setup

#define PIN_BULB      25
#define PIN_FAN       26
#define PIN_BED_LIGHT 27
#define PIN_CHARGING  14

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ================================================================
//  GLOBALS
// ================================================================

WebSocketsClient wsClient;
Preferences      prefs;
WebServer        configServer(80);
DNSServer        dnsServer;

struct DeviceStates {
    bool bulb     = false;
    bool fan      = false;
    bool bedLight = false;
    bool charging = false;
} dev;

bool          serverConnected  = false;
int           reconnectCount   = 0;
unsigned long lastStatusSend   = 0;
unsigned long lastWifiCheck    = 0;
bool          credentialsSaved = false;

String wifiSSID;
String wifiPass;

// ================================================================
//  PRINT HELPERS
// ================================================================

void printSeparator() {
    Serial.println(F("------------------------------------------"));
}

void printDeviceTable() {
    Serial.println(F("  ┌─────────────┬────────┐"));
    Serial.printf( "  │ Bulb        │  %s   │\n", dev.bulb     ? " ON " : " OFF");
    Serial.printf( "  │ Fan         │  %s   │\n", dev.fan      ? " ON " : " OFF");
    Serial.printf( "  │ Bed Light   │  %s   │\n", dev.bedLight ? " ON " : " OFF");
    Serial.printf( "  │ Charging    │  %s   │\n", dev.charging ? " ON " : " OFF");
    Serial.println(F("  └─────────────┴────────┘"));
}

// ================================================================
//  RELAY CONTROL
// ================================================================

void setRelay(uint8_t pin, bool on) {
    digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
    Serial.printf("  [GPIO %d] → %s\n", pin, on ? "ON  ⚡" : "OFF ○");
}

// ================================================================
//  STATE PERSISTENCE
// ================================================================

void saveStates() {
    prefs.begin("smarthome", false);
    prefs.putBool("bulb",     dev.bulb);
    prefs.putBool("fan",      dev.fan);
    prefs.putBool("bedLight", dev.bedLight);
    prefs.putBool("charging", dev.charging);
    prefs.end();
    Serial.println("  [FLASH] States saved ✓");
}

void loadStates() {
    prefs.begin("smarthome", true);
    dev.bulb     = prefs.getBool("bulb",     false);
    dev.fan      = prefs.getBool("fan",      false);
    dev.bedLight = prefs.getBool("bedLight", false);
    dev.charging = prefs.getBool("charging", false);
    prefs.end();
    setRelay(PIN_BULB,      dev.bulb);
    setRelay(PIN_FAN,       dev.fan);
    setRelay(PIN_BED_LIGHT, dev.bedLight);
    setRelay(PIN_CHARGING,  dev.charging);
    Serial.println("[STATE] Loaded from flash:");
    printDeviceTable();
}

// ================================================================
//  SEND STATUS TO SERVER
// ================================================================

void sendStatus() {
    if (!serverConnected) {
        Serial.println("  [WS] Not connected — status not sent");
        return;
    }
    StaticJsonDocument<192> doc;
    doc["bulb"]      = dev.bulb;
    doc["fan"]       = dev.fan;
    doc["bed_light"] = dev.bedLight;
    doc["charging"]  = dev.charging;
    String json;
    serializeJson(doc, json);
    wsClient.sendTXT(json);
    Serial.printf("  [WS] Status sent → %s\n", json.c_str());
}

// ================================================================
//  DEVICE CONTROL
// ================================================================

void controlDevice(const String& device, const String& action) {
    bool on = (action == "on");
    printSeparator();
    Serial.printf("[COMMAND] Device: %-10s  Action: %s\n", device.c_str(), action.c_str());

    if (device == "bulb") {
        dev.bulb = on;     setRelay(PIN_BULB,      on);
    } else if (device == "fan") {
        dev.fan = on;      setRelay(PIN_FAN,        on);
    } else if (device == "bed_light") {
        dev.bedLight = on; setRelay(PIN_BED_LIGHT,  on);
    } else if (device == "charging") {
        dev.charging = on; setRelay(PIN_CHARGING,   on);
    } else if (device == "all") {
        dev.bulb = dev.fan = dev.bedLight = dev.charging = on;
        setRelay(PIN_BULB, on); setRelay(PIN_FAN, on);
        setRelay(PIN_BED_LIGHT, on); setRelay(PIN_CHARGING, on);
    } else {
        Serial.printf("  [WARN] Unknown device: %s\n", device.c_str());
        return;
    }
    printDeviceTable();
    saveStates();
    sendStatus();
}

void applyScene(const String& scene) {
    printSeparator();
    Serial.printf("[SCENE] Applying: %s\n", scene.c_str());

    if (scene == "sleep") {
        dev.bulb = dev.fan = dev.bedLight = false;
        setRelay(PIN_BULB, false); setRelay(PIN_FAN, false); setRelay(PIN_BED_LIGHT, false);
    } else if (scene == "movie") {
        dev.bulb = false; dev.fan = true; dev.bedLight = false;
        setRelay(PIN_BULB, false); setRelay(PIN_FAN, true); setRelay(PIN_BED_LIGHT, false);
    } else if (scene == "reading") {
        dev.bulb = false; dev.fan = true; dev.bedLight = true;
        setRelay(PIN_BULB, false); setRelay(PIN_FAN, true); setRelay(PIN_BED_LIGHT, true);
    } else {
        Serial.printf("  [WARN] Unknown scene: %s\n", scene.c_str());
        return;
    }
    printDeviceTable();
    saveStates();
    sendStatus();
}

// ================================================================
//  PROCESS INCOMING WS COMMAND
// ================================================================

void handleCommand(const String& raw) {
    Serial.println("[WS] Parsing command...");
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, raw)) {
        Serial.println("  [ERROR] Invalid JSON — ignoring");
        return;
    }
    if (doc.containsKey("type")) {
        Serial.printf("  [INFO] Server message: %s\n", doc["type"].as<String>().c_str());
        return;
    }
    if (doc.containsKey("error")) {
        Serial.printf("  [AI] Error: %s\n", doc["msg"] | "unknown");
        return;
    }
    if (doc.containsKey("scene")) {
        applyScene(doc["scene"].as<String>());
    } else if (doc.containsKey("commands")) {
        JsonArray cmds = doc["commands"].as<JsonArray>();
        Serial.printf("  [MULTI] %d command(s)\n", cmds.size());
        for (JsonObject cmd : cmds) {
            controlDevice(cmd["device"].as<String>(), cmd["action"].as<String>());
            delay(60);
        }
    } else if (doc.containsKey("device") && doc.containsKey("action")) {
        controlDevice(doc["device"].as<String>(), doc["action"].as<String>());
    } else {
        Serial.printf("  [WARN] Unrecognised format: %s\n", raw.c_str());
    }
}

// ================================================================
//  WEBSOCKET EVENTS
// ================================================================

void onWSEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            serverConnected = false;
            reconnectCount++;
            printSeparator();
            Serial.printf("[WS] ✗ Disconnected  (retry #%d)\n", reconnectCount);
            break;

        case WStype_CONNECTED:
            serverConnected = true;
            reconnectCount  = 0;
            printSeparator();
            Serial.println("[WS] ✓ Connected to server!");
            Serial.printf("  Server : %s:%d\n", SERVER_HOST, SERVER_PORT);
            Serial.printf("  Room   : %s\n", ROOM_ID);
            sendStatus();
            break;

        case WStype_TEXT:
            printSeparator();
            Serial.printf("[WS] ← Received (%d bytes):\n  %s\n", length, (char*)payload);
            handleCommand(String((char*)payload));
            break;

        case WStype_PING: Serial.println("[WS] ← PING"); break;
        case WStype_PONG: Serial.println("[WS] → PONG"); break;

        case WStype_ERROR:
            Serial.printf("[WS] ✗ Error — code: %u\n", (uint16_t)(size_t)payload);
            break;

        default: break;
    }
}

// ================================================================
//  WIFI CONFIG PORTAL  (shown when no saved credentials / can't connect)
// ================================================================

// Escape special HTML chars in SSID names
String htmlEsc(const String& s) {
    String r = s;
    r.replace("&", "&amp;");
    r.replace("\"", "&quot;");
    r.replace("<", "&lt;");
    r.replace(">", "&gt;");
    return r;
}

// Signal strength → visual bar string
String signalBars(int rssi) {
    if (rssi >= -50) return "▂▄▆█";
    if (rssi >= -60) return "▂▄▆&nbsp;";
    if (rssi >= -70) return "▂▄&nbsp;&nbsp;";
    return                  "▂&nbsp;&nbsp;&nbsp;";
}

String buildNetworkList() {
    Serial.println("[PORTAL] Scanning nearby WiFi networks...");
    int n = WiFi.scanNetworks();
    Serial.printf("[PORTAL] Found %d networks\n", n);

    String html = "";
    for (int i = 0; i < n; i++) {
        String ssidEsc = htmlEsc(WiFi.SSID(i));
        bool   locked  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        int    rssi    = WiFi.RSSI(i);

        html += "<div class='net' data-ssid=\"" + ssidEsc + "\">";
        html += "<div class='net-left'>";
        html += "<span class='lock'>" + String(locked ? "&#128274; " : "") + "</span>";
        html += "<span class='ssid-name'>" + ssidEsc + "</span>";
        html += "</div>";
        html += "<span class='bars'>" + signalBars(rssi) + " <small>" + String(rssi) + " dBm</small></span>";
        html += "</div>";
    }
    WiFi.scanDelete();

    if (n == 0) {
        html = "<div style='padding:16px;text-align:center;color:#8b949e'>No networks found.<br>"
               "<a href='/' style='color:#58a6ff'>Tap to refresh</a></div>";
    }
    return html;
}

void servePortalPage() {
    String networks = buildNetworkList();

    String html = F(R"(<!DOCTYPE html><html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>SmartHomeAI WiFi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#0d1117;color:#e6edf3;
     min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:#161b22;border:1px solid #30363d;border-radius:16px;
      padding:26px;width:100%;max-width:420px}
h1{font-size:21px;color:#58a6ff;margin-bottom:4px}
.sub{font-size:13px;color:#8b949e;margin-bottom:18px}
.section-title{font-size:12px;color:#8b949e;text-transform:uppercase;
               letter-spacing:.05em;margin-bottom:8px}
.net-wrap{max-height:230px;overflow-y:auto;border:1px solid #30363d;
          border-radius:10px;margin-bottom:18px}
.net{display:flex;justify-content:space-between;align-items:center;
     padding:12px 14px;cursor:pointer;border-bottom:1px solid #21262d;
     transition:background .15s;user-select:none}
.net:last-child{border-bottom:none}
.net:hover{background:#1c2128}
.net.sel{background:#1f6feb}
.net-left{display:flex;align-items:center;gap:4px}
.ssid-name{font-size:14px;font-weight:600}
.bars{font-size:13px;font-family:monospace;color:#8b949e;white-space:nowrap}
.net.sel .bars{color:#cde}
label{display:block;font-size:12px;color:#8b949e;margin-bottom:5px}
input{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #30363d;
      background:#0d1117;color:#e6edf3;font-size:14px;margin-bottom:14px}
input:focus{outline:none;border-color:#58a6ff;box-shadow:0 0 0 3px rgba(88,166,255,.15)}
.btn{display:block;width:100%;padding:12px;border-radius:8px;border:none;
     background:#238636;color:#fff;font-size:15px;font-weight:700;cursor:pointer;margin-top:6px}
.btn:hover{background:#2ea043}
.refresh{display:block;text-align:center;margin-top:14px;font-size:12px;
         color:#58a6ff;cursor:pointer;text-decoration:none}
</style></head><body><div class='card'>
<h1>SmartHomeAI WiFi</h1>
<p class='sub'>Select your network, then enter the password</p>
<p class='section-title'>Nearby Networks</p>
<div class='net-wrap' id='nets'>)");

    html += networks;

    html += F(R"(</div>
<form method='POST' action='/save'>
<label>WiFi Name</label>
<input type='text'     name='ssid' id='ssid' placeholder='Tap a network above or type here' required autocomplete='off'>
<label>Password</label>
<input type='password' name='pass' id='pass' placeholder='Enter WiFi password' autocomplete='off'>
<button class='btn' type='submit'>Connect &#8594;</button>
</form>
<a class='refresh' href='/'>&#8635; Refresh network list</a>
</div>
<script>
document.querySelectorAll('.net').forEach(function(el){
  el.addEventListener('click',function(){
    document.querySelectorAll('.net').forEach(function(e){e.classList.remove('sel');});
    el.classList.add('sel');
    document.getElementById('ssid').value = el.dataset.ssid;
    document.getElementById('pass').focus();
  });
});
</script>
</body></html>)");

    configServer.send(200, "text/html", html);
}

void handlePortalSave() {
    if (!configServer.hasArg("ssid") || configServer.arg("ssid").length() == 0) {
        configServer.send(400, "text/plain", "Missing SSID");
        return;
    }

    wifiSSID = configServer.arg("ssid");
    wifiPass = configServer.arg("pass");

    // Persist to flash
    prefs.begin("wifi", false);
    prefs.putString("ssid", wifiSSID);
    prefs.putString("pass", wifiPass);
    prefs.end();

    Serial.printf("[PORTAL] Saved — SSID: %s\n", wifiSSID.c_str());

    String escaped = htmlEsc(wifiSSID);
    String html = "<!DOCTYPE html><html><head>"
      "<meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Connecting...</title>"
      "<style>"
      "body{font-family:Arial,sans-serif;background:#0d1117;color:#e6edf3;"
      "display:flex;align-items:center;justify-content:center;height:100vh;text-align:center;padding:20px}"
      ".box{background:#161b22;border:1px solid #30363d;border-radius:16px;padding:32px;max-width:360px;width:100%}"
      "h2{color:#3fb950;margin-bottom:12px;font-size:22px}"
      "p{color:#8b949e;font-size:14px;line-height:1.7}"
      ".ssid{color:#58a6ff;font-weight:bold}"
      ".dot{display:inline-block;animation:blink 1s infinite}.dot:nth-child(2){animation-delay:.2s}.dot:nth-child(3){animation-delay:.4s}"
      "@keyframes blink{0%,80%,100%{opacity:0}40%{opacity:1}}"
      "</style></head><body>"
      "<div class='box'>"
      "<h2>&#10003; Saved!</h2>"
      "<p>Connecting to<br><span class='ssid'>" + escaped + "</span><br><br>"
      "ESP32 is restarting<span class='dot'>.</span><span class='dot'>.</span><span class='dot'>.</span><br>"
      "<small>Reconnect your phone to your home WiFi.</small></p>"
      "</div></body></html>";

    configServer.send(200, "text/html", html);
    delay(800);  // make sure response is flushed
    credentialsSaved = true;
}

void startConfigPortal() {
    printSeparator();
    Serial.println("[PORTAL] No WiFi — starting setup hotspot");
    Serial.printf( "[PORTAL] Hotspot SSID : %s\n", AP_SSID);
    Serial.println("[PORTAL] Connect your phone and open  http://192.168.4.1");
    printSeparator();

    WiFi.mode(WIFI_AP_STA);   // AP+STA allows network scanning while hosting AP
    WiFi.softAP(AP_SSID);
    delay(500);

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);   // redirect ALL DNS to our IP (captive portal)

    configServer.on("/",     HTTP_GET,  servePortalPage);
    configServer.on("/save", HTTP_POST, handlePortalSave);
    configServer.onNotFound(servePortalPage);  // catches captive-portal detection URLs
    configServer.begin();

    credentialsSaved = false;
    while (!credentialsSaved) {
        dnsServer.processNextRequest();
        configServer.handleClient();
        delay(2);
    }

    configServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);

    Serial.println("[PORTAL] Credentials received — restarting ESP32...");
    delay(1500);
    ESP.restart();
}

// ================================================================
//  WIFI CONNECT
// ================================================================

void connectWiFi(bool isReconnect = false) {
    // Load saved credentials
    prefs.begin("wifi", true);
    wifiSSID = prefs.getString("ssid", "");
    wifiPass = prefs.getString("pass", "");
    prefs.end();

    if (wifiSSID.length() == 0) {
        startConfigPortal();
        return;
    }

    Serial.printf("[WIFI] Connecting to \"%s\"", wifiSSID.c_str());

    // Clean-start sequence — fixes slow connect on cold boot.
    // ESP32 stores the last AP in its own WiFi NVS and tries to reconnect to
    // it on every boot, racing against our WiFi.begin(). After a manual reset
    // that cache is already gone, which is why reset always connects faster.
    // disconnect(true,true) erases that cached AP so cold boot behaves the same.
    WiFi.persistent(false);         // 1. must be first — stop WiFi writing to flash
    WiFi.setAutoReconnect(false);   // 2. we drive reconnection manually
    WiFi.disconnect(true, true);    // 3. full disconnect + erase cached AP from WiFi NVS
    delay(200);                     // 4. let radio fully reset before re-init
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);  // ensure max TX power (sometimes low on cold boot)
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - start > 15000) {
            if (isReconnect) {
                Serial.println("\n[WIFI] ✗ Reconnect failed — restarting...");
                delay(1000);
                ESP.restart();
            } else {
                Serial.println("\n[WIFI] ✗ Cannot connect — re-opening setup portal");
                startConfigPortal();
            }
            return;
        }
    }

    // Re-enable auto-reconnect now that we are connected
    WiFi.setAutoReconnect(true);

    delay(300);  // let network stack fully settle before opening WebSocket

    Serial.println();
    printSeparator();
    Serial.println("[WIFI] ✓ Connected!");
    Serial.printf("  SSID    : %s\n",  WiFi.SSID().c_str());
    Serial.printf("  IP      : %s\n",  WiFi.localIP().toString().c_str());
    Serial.printf("  Gateway : %s\n",  WiFi.gatewayIP().toString().c_str());
    Serial.printf("  Signal  : %d dBm\n", WiFi.RSSI());
    printSeparator();
}

// ================================================================
//  CONNECT TO SERVER
// ================================================================

void connectToServer() {
    String path = String("/ws?room=") + ROOM_ID + "&type=esp32";

    Serial.println("[WS] Connecting to server...");
    Serial.printf("  Host : %s\n", SERVER_HOST);
    Serial.printf("  Port : %d\n", SERVER_PORT);
    Serial.printf("  Path : %s\n", path.c_str());
    Serial.printf("  SSL  : %s\n", USE_SSL ? "YES" : "NO");

    if (USE_SSL) {
        wsClient.beginSSL(SERVER_HOST, SERVER_PORT, path.c_str());
    } else {
        wsClient.begin(SERVER_HOST, SERVER_PORT, path.c_str());
    }
    wsClient.onEvent(onWSEvent);
    wsClient.setReconnectInterval(3000);
    wsClient.enableHeartbeat(15000, 5000, 3);
}

// ================================================================
//  SETUP
// ================================================================

void setup() {
    Serial.begin(115200);
    delay(300);

    Serial.println();
    Serial.println(F("=========================================="));
    Serial.println(F("     SmartHomeAI — ESP32 Controller"));
    Serial.println(F("=========================================="));
    Serial.printf( "  Server : %s:%d\n", SERVER_HOST, SERVER_PORT);
    Serial.printf( "  Room   : %s\n", ROOM_ID);
    Serial.println(F("=========================================="));

    // Init relay pins — all OFF
    Serial.println("\n[RELAY] Initialising pins...");
    pinMode(PIN_BULB,      OUTPUT); setRelay(PIN_BULB,      false);
    pinMode(PIN_FAN,       OUTPUT); setRelay(PIN_FAN,       false);
    pinMode(PIN_BED_LIGHT, OUTPUT); setRelay(PIN_BED_LIGHT, false);
    pinMode(PIN_CHARGING,  OUTPUT); setRelay(PIN_CHARGING,  false);
    Serial.println("[RELAY] All relays OFF ✓");

    Serial.println();
    loadStates();

    Serial.println();
    connectWiFi();       // loads creds; starts portal if missing/wrong

    Serial.println();
    connectToServer();

    Serial.println();
    Serial.println(F("=========================================="));
    Serial.println(F("  Ready! Waiting for voice commands..."));
    Serial.println(F("=========================================="));
    Serial.println();
}

// ================================================================
//  LOOP
// ================================================================

void loop() {
    wsClient.loop();

    // WiFi watchdog — every 10 s
    if (millis() - lastWifiCheck > 10000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            printSeparator();
            Serial.println("[WIFI] ✗ Connection lost — reconnecting...");
            connectWiFi(true);   // reconnect mode: restarts on failure, no portal
        }
    }

    // Heartbeat — every 30 s
    if (millis() - lastStatusSend > 30000) {
        lastStatusSend = millis();
        printSeparator();
        Serial.println("[♥] Heartbeat");
        Serial.printf("  Uptime  : %lu s\n",    millis() / 1000);
        Serial.printf("  WiFi    : %s (%d dBm)\n", WiFi.SSID().c_str(), WiFi.RSSI());
        Serial.printf("  Server  : %s\n",
                      serverConnected ? "✓ Connected" : "✗ Disconnected");
        printDeviceTable();
        sendStatus();
    }
}
