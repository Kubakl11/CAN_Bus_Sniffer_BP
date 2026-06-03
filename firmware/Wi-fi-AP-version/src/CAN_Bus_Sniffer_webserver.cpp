//project/
//├── src/
//│   └── CAN_Bus_Sniffer_webserver.cpp    
//├── data/                 
//│   ├── dashboard.html
//│   ├── alarms.html
//│   ├── consumption.html
//│   ├── graphs.html
//│   ├── vehicle-data.html
//│   ├── diagnostic.html
//│   ├── login.html
//│   ├── style.css
//│   └── data.js
//│   └── chart.min.js
//│   └── chartsjs-plugin-zoom.js
//│   └── hammer.min.js
//└── platformio.ini

// ═══════════════════════════════════════════════════════════════════════
//  OBD-II Dashboard Server
//  ESP32-S3 + MCP2518FD  |  CAN 2.0 @ 500 kbit/s
//  WPA2 WiFi AP + DNS Captive Portal + /api/data JSON endpoint
// ═══════════════════════════════════════════════════════════════════════



#include <Arduino.h>
#include <ACAN2517FD.h>
#include <SPI.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>

// ── SPI piny ─────────────────────────────────────────────────────────
static const byte MCP2518FD_CS  = 10;
static const byte MCP2518FD_INT = 47;
static const byte SPI_SCK       = 12;
static const byte SPI_MISO      = 13;
static const byte SPI_MOSI      = 11;

// ── LED a tlačítko ───────────────────────────────────────────────────
static const byte LED_STATUS1 = 1;  // svítí po WiFi AP init
static const byte LED_STATUS2 = 2;  // svítí po scan dokončení (LOOP aktivní)
static const byte LED_CAN     = 3;  // bliká při každém CAN frame
static const byte BUTTON_PIN  = 4;

// ── WiFi ─────────────────────────────────────────────────────────────
#define WIFI_SSID     "CANBus_Sniffer_v1"
#define WIFI_PASS     "obd321aa"

static const byte DNS_PORT = 53;
DNSServer  dnsServer;
WebServer  server(80);

ACAN2517FD can(MCP2518FD_CS, SPI, MCP2518FD_INT);
void IRAM_ATTR canISR() { can.isr(); }

// ═══════════════════════════════════════════════════════════════════════
//  PID TABULKA
struct OBDPid {
    uint8_t     pid;
    const char* jsonKey;
    bool        supported;
    bool        hasValue;  
    float       lastValue;
    uint8_t     rawA;
    uint8_t     rawB;
};

OBDPid pidList[] = {
    // ── CORE ────────────────────────────────────────────────────────
    {0x0D, "speed",           false, false, 0, 0, 0},
    {0x0C, "rpm",             false, false, 0, 0, 0},
    {0x05, "coolantTemp",     false, false, 0, 0, 0},
    {0x11, "throttle",        false, false, 0, 0, 0},
    {0x1F, "engineRuntime",   false, false, 0, 0, 0},
    // ── ENGINE ──────────────────────────────────────────────────────
    {0x04, "engineLoad",      false, false, 0, 0, 0},
    {0x06, "stft",            false, false, 0, 0, 0},
    {0x07, "ltft",            false, false, 0, 0, 0},
    {0x0B, "mapPressure",     false, false, 0, 0, 0},
    {0x0E, "ignitionTiming",  false, false, 0, 0, 0},
    {0x0F, "intakeAirTemp",   false, false, 0, 0, 0},
    {0x10, "maf",             false, false, 0, 0, 0},
    {0x46, "outdoorTemp",     false, false, 0, 0, 0},
    {0x5C, "oilTemp",         false, false, 0, 0, 0},
    {0x43, "absoluteLoad",    false, false, 0, 0, 0},
    // ── FUEL ────────────────────────────────────────────────────────
    {0x2F, "fuelLevel",       false, false, 0, 0, 0},
    {0x5E, "fuelConsumption", false, false, 0, 0, 0},
    {0x0A, "fuelPressure",    false, false, 0, 0, 0},
    // ── EMISE ───────────────────────────────────────────────────────
    {0x21, "milDistance",     false, false, 0, 0, 0},
    {0x31, "distSinceClr",    false, false, 0, 0, 0},
    {0x14, "o2Voltage",       false, false, 0, 0, 0},
    {0x3C, "catalystTemp",    false, false, 0, 0, 0},
    {0x33, "barometric",      false, false, 0, 0, 0},
    // ── ELEKTRO / HEV ───────────────────────────────────────────────
    {0x42, "systemVoltage",   false, false, 0, 0, 0},
    {0x5B, "hvBatterySoc",    false, false, 0, 0, 0},
    {0x59, "fuelRailPress",   false, false, 0, 0, 0},
    {0x9C, "hvBatteryTemp",   false, false, 0, 0, 0},
    {0x9D, "hvBatteryVolt",   false, false, 0, 0, 0},
    {0x9E, "hvBatteryCurr",   false, false, 0, 0, 0},
};
const uint8_t PID_COUNT = sizeof(pidList) / sizeof(pidList[0]);

// ── Stav ─────────────────────────────────────────────────────────────
bool          scanning    = false;
bool          looping     = false;
uint8_t       loopIndex   = 0;
unsigned long lastRequest  = 0;
unsigned long lastCanLedOn = 0;
String        inputBuffer  = "";

// ═══════════════════════════════════════════════════════════════════════
//  OBD FUNKCE 
// ═══════════════════════════════════════════════════════════════════════

void sendOBDRequest(uint8_t pid) {
    CANFDMessage frame;
    frame.id      = 0x7DF;
    frame.len     = 8;
    frame.data[0] = 0x02;
    frame.data[1] = 0x01;
    frame.data[2] = pid;
    frame.data[3] = 0x00;
    frame.data[4] = 0x00;
    frame.data[5] = 0x00;
    frame.data[6] = 0x00;
    frame.data[7] = 0x00;
    can.tryToSend(frame);
}

// Dekódování 
float decodePID(uint8_t pid, uint8_t a, uint8_t b) {
    switch (pid) {
        case 0x04: return (a * 100.0) / 255.0;
        case 0x05: return a - 40.0;
        case 0x06: return (a - 128.0) * 100.0 / 128.0;
        case 0x07: return (a - 128.0) * 100.0 / 128.0;
        case 0x0A: return a * 3.0;
        case 0x0B: return a;
        case 0x0C: return ((a * 256.0) + b) / 4.0;
        case 0x0D: return a;
        case 0x0E: return (a / 2.0) - 64.0;
        case 0x0F: return a - 40.0;
        case 0x10: return ((a * 256.0) + b) / 100.0;
        case 0x11: return (a * 100.0) / 255.0;
        case 0x1F: return (a * 256.0) + b;
        case 0x21: return (a * 256.0) + b;
        case 0x2F: return (a * 100.0) / 255.0;
        case 0x31: return (a * 256.0) + b;
        case 0x33: return a;
        case 0x3C: return ((a * 256.0) + b) / 10.0 - 40.0;
        case 0x42: return ((a * 256.0) + b) / 1000.0;
        case 0x43: return (a * 100.0) / 255.0;
        case 0x46: return a - 40.0;
        case 0x4D: return (a * 256.0) + b;
        case 0x59: return ((a * 256.0) + b) * 10.0;
        case 0x5B: return (a * 100.0) / 255.0;
        case 0x5C: return a - 40.0;
        case 0x5E: return ((a * 256.0) + b) / 20.0;
        case 0x9C: return a - 40.0;
        case 0x9D: return ((a * 256.0) + b) / 10.0;
        case 0x9E: return ((a * 256.0) + b) / 10.0 - 3276.8;
        default:   return a;
    }
}

// Zpracování scan bitmask
void processSupportedPIDs(uint8_t basePid, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint32_t bitmask = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
    for (uint8_t i = 0; i < 32; i++) {
        if (bitmask & (1UL << (31 - i))) {
            uint8_t supportedPid = basePid + i + 1;
            for (uint8_t j = 0; j < PID_COUNT; j++) {
                if (pidList[j].pid == supportedPid) {
                    pidList[j].supported = true;
                }
            }
        }
    }
}

// Zpracování CAN rámce 
void processOBDResponse(CANFDMessage &frame) {
    if (frame.id < 0x7E8 || frame.id > 0x7EF) return;
    if (frame.data[1] != 0x41) return;

    uint8_t pid = frame.data[2];

    // Scan bitmask odpovědi
    if (pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60 || pid == 0x80) {
        processSupportedPIDs(pid, frame.data[3], frame.data[4], frame.data[5], frame.data[6]);
        if (scanning) {
            if      (pid == 0x00) sendOBDRequest(0x20);
            else if (pid == 0x20) sendOBDRequest(0x40);
            else if (pid == 0x40) sendOBDRequest(0x60);
            else if (pid == 0x60) sendOBDRequest(0x80);
            else {
                // Scan dokončen (odpověď na 0x80)
                scanning  = false;
                looping   = true;
                loopIndex = 0;
                lastRequest = 0;
                digitalWrite(LED_STATUS2, HIGH);
                Serial.println("[OBD] Scan hotov, LOOP spusten");
                for (uint8_t i = 0; i < PID_COUNT; i++) {
                    if (pidList[i].supported) {
                        Serial.print("  OK: ");
                        Serial.println(pidList[i].jsonKey);
                    }
                }
            }
        }
        return;
    }

    // Normální datová odpověď
    uint8_t a = frame.data[3];
    uint8_t b = frame.data[4];
    float value = decodePID(pid, a, b);

    for (uint8_t i = 0; i < PID_COUNT; i++) {
        if (pidList[i].pid == pid) {
            pidList[i].lastValue = value;
            pidList[i].rawA      = a;
            pidList[i].rawB      = b;
            pidList[i].hasValue  = true;
            break;
        }
    }
}

// Najde index dalšího podporovaného PIDu
int8_t nextSupportedPid(uint8_t from) {
    for (uint8_t i = from; i < PID_COUNT; i++) {
        if (pidList[i].supported) return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════
//  WEB SERVER
// ═══════════════════════════════════════════════════════════════════════

void handleCaptivePortal() {
    String ip = WiFi.softAPIP().toString();
    server.sendHeader("Location", "http://" + ip + "/", true);
    server.send(302, "text/plain", "");
}

// /api/data → JSON
// Podporované PIDy = číslo, nepodporované nebo bez dat = null
// data.js předá výsledek do getData() → dashboard skryje null hodnoty
void handleApiData() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-cache");

    String json = "{";
    bool first = true;

    for (uint8_t i = 0; i < PID_COUNT; i++) {
        if (!pidList[i].supported || !pidList[i].hasValue) continue;

        if (!first) json += ",";
        first = false;

        json += "\"";
        json += pidList[i].jsonKey;
        json += "\":";

        char buf[16];
        dtostrf(pidList[i].lastValue, 1, 2, buf);
        json += buf;
    }

    // Status pro debug
    if (!first) json += ",";
    json += "\"_scanning\":";
    json += scanning ? "true" : "false";
    json += ",\"_looping\":";
    json += looping ? "true" : "false";
    json += ",\"_uptime\":";
    json += millis() / 1000;
    json += "}";

    server.send(200, "application/json", json);
}

// /api/cmd?c=SCAN|LOOP|STOP|READ
void handleApiCmd() {
    server.sendHeader("Access-Control-Allow-Origin", "*");

    String cmd = server.arg("c");
    cmd.toUpperCase();

    if (cmd == "SCAN") {
        scanning = true;
        looping  = false;
        for (uint8_t i = 0; i < PID_COUNT; i++) {
            pidList[i].supported = false;
            pidList[i].hasValue  = false;
        }
        sendOBDRequest(0x00);
        Serial.println("[CMD] SCAN");
    }
    else if (cmd == "LOOP") {
        looping   = true;
        loopIndex = 0;
        lastRequest = 0;
        digitalWrite(LED_STATUS2, HIGH);
        Serial.println("[CMD] LOOP");
    }
    else if (cmd == "STOP") {
        looping  = false;
        scanning = false;
        digitalWrite(LED_STATUS2, LOW);
        Serial.println("[CMD] STOP");
    }
    else if (cmd == "READ") {
        looping = false;
        for (uint8_t i = 0; i < PID_COUNT; i++) {
            if (pidList[i].supported) {
                sendOBDRequest(pidList[i].pid);
                delay(40);
            }
        }
        Serial.println("[CMD] READ");
    }

    server.send(200, "text/plain", "OK");
}

// ═══════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(2000);

    Serial.println("==================================");
    Serial.println("  OBD-II Dashboard Server v3.0");
    Serial.println("  ESP32-S3 + MCP2518FD");
    Serial.println("==================================");

    pinMode(LED_STATUS1, OUTPUT);
    pinMode(LED_STATUS2, OUTPUT);
    pinMode(LED_CAN,     OUTPUT);
    pinMode(BUTTON_PIN,  INPUT_PULLUP);

    // LittleFS.begin
    if (!LittleFS.begin(true)) {
        Serial.println("[LittleFS] CHYBA");
    } else {
        Serial.println("[LittleFS] OK");
        File root = LittleFS.open("/");
        File file = root.openNextFile();
        while (file) {
            Serial.print("[LittleFS] soubor: ");
            Serial.println(file.name());
            file = root.openNextFile();
        }
    }
    // ── WiFi AP ──
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    delay(500);

    IPAddress apIP = WiFi.softAPIP();
    Serial.print("[WiFi] http://"); Serial.println(apIP);
    digitalWrite(LED_STATUS1, HIGH);

    // ── DNS Captive Portal ──
    dnsServer.start(DNS_PORT, "*", apIP);
    Serial.println("[DNS]  Captive Portal OK");

    // 1. Captive portal OS probes — PRVNÍ
    server.on("/generate_204",              handleCaptivePortal);
    server.on("/connecttest.txt",           handleCaptivePortal);
    server.on("/hotspot-detect.html",       handleCaptivePortal);
    server.on("/library/test/success.html", handleCaptivePortal);
    server.on("/ncsi.txt",                  handleCaptivePortal);
    server.on("/success.txt",               handleCaptivePortal);

    // 2. Root redirect
    server.on("/", []() {
        server.sendHeader("Location", "/dashboard.html", true);
        server.send(302, "text/plain", "");
    });

    // 3. API endpointy
    server.on("/api/data", handleApiData);
    server.on("/api/cmd",  handleApiCmd);

    // 4. Statické soubory - LittleFS
    server.serveStatic("/hammer.min.js",                LittleFS, "/hammer.min.js");
    server.serveStatic("/chartsjs-plugin-zoom.min.js",   LittleFS, "/chartsjs-plugin-zoom.min.js");
    server.serveStatic("/chart.min.js",                 LittleFS, "/chart.min.js");
    server.serveStatic("/leaflet.js",                   LittleFS, "/leaflet.js");
    server.serveStatic("/leaflet.css",                  LittleFS, "/leaflet.css");
    server.serveStatic("/dashboard.html",               LittleFS, "/dashboard.html");
    server.serveStatic("/alarms.html",       LittleFS, "/alarms.html");
    server.serveStatic("/consumption.html",  LittleFS, "/consumption.html");
    server.serveStatic("/graphs.html",       LittleFS, "/graphs.html");
    server.serveStatic("/vehicle-data.html", LittleFS, "/vehicle-data.html");
    server.serveStatic("/diagnostic.html",   LittleFS, "/diagnostic.html");
    server.serveStatic("/login.html",        LittleFS, "/login.html");
    server.serveStatic("/style.css",         LittleFS, "/style.css");
    server.serveStatic("/data.js",           LittleFS, "/data.js");

    // 5. Fallback
    server.onNotFound([]() {
        Serial.print("[HTTP] 404: ");
        Serial.println(server.uri());
        handleCaptivePortal();
    });

    server.begin();
    Serial.println("[HTTP] Web server OK");

    // ── CAN --
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    ACAN2517FDSettings settings(
        ACAN2517FDSettings::OSC_40MHz,
        500 * 1000,
        DataBitRateFactor::x1
    );
    settings.mRequestedMode = ACAN2517FDSettings::Normal20B;

    const uint32_t err = can.begin(settings, canISR);
    if (err == 0) {
        Serial.println("[CAN]  OK @ 500 kbit/s");
    } else {
        Serial.print("[CAN]  CHYBA: 0x");
        Serial.println(err, HEX);
    }

    // ── Automatický OBD scan při startu ──
    Serial.println("[OBD]  Spoustim scan...");
    scanning = true;
    sendOBDRequest(0x00);

    Serial.println("==================================");
}

// ═══════════════════════════════════════════════════════════════════════
//  LOOP 
// ═══════════════════════════════════════════════════════════════════════
void loop() {
    // ── Scan timeout ─────────────────────────────────────────────
    static unsigned long scanStart = 0;
    if (scanning && scanStart == 0) scanStart = millis();
    if (scanning && millis() - scanStart > 5000) {
        scanning  = false;
        looping   = true;
        loopIndex = 0;
        scanStart = 0;
        Serial.println("[OBD] Scan timeout → LOOP (zkousim vsechny PIDy)");
        for (uint8_t i = 0; i < PID_COUNT; i++) {
            pidList[i].supported = true;
        }
    }

    // ── DEBUG: 1× za sekundu výpis stavu ─────────────────────────
    static unsigned long lastDebugPrint = 0;
    if (millis() - lastDebugPrint > 1000) {
        lastDebugPrint = millis();
        Serial.print("[STATE] scan="); Serial.print(scanning);
        Serial.print(" loop=");        Serial.print(looping);
        Serial.print(" | ");
        for (uint8_t i = 0; i < PID_COUNT; i++) {
            if (pidList[i].hasValue) {
                Serial.print(pidList[i].jsonKey);
                Serial.print("=");
                Serial.print(pidList[i].lastValue, 1);
                Serial.print(" ");
            }
        }
        Serial.println();
    }

    dnsServer.processNextRequest();
    server.handleClient();

    // ── Serial příkazy ──────────────────────────────────────────
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            inputBuffer.trim();
            inputBuffer.toUpperCase();
            if      (inputBuffer == "SCAN") { scanning = true; looping = false; for (uint8_t i = 0; i < PID_COUNT; i++) { pidList[i].supported = false; pidList[i].hasValue = false; } sendOBDRequest(0x00); }
            else if (inputBuffer == "LOOP") { looping = true; loopIndex = 0; lastRequest = 0; }
            else if (inputBuffer == "STOP") { looping = false; scanning = false; }
            inputBuffer = "";
        } else {
            inputBuffer += c;
        }
    }

    // ── LOOP — odesílání requestů (interval 50 ms) ─────────────
    if (looping && millis() - lastRequest >= 50) {
        lastRequest = millis();
        int8_t idx = nextSupportedPid(loopIndex);
        if (idx >= 0) {
            Serial.print("TX 0x"); Serial.println(pidList[idx].pid, HEX);
            sendOBDRequest(pidList[idx].pid);
            loopIndex = idx + 1;
        } else {
            loopIndex = 0;
        }
    }

    // ── CAN receive ─────────────────────────────────────────────
    CANFDMessage rxFrame;
    if (can.receive(rxFrame)) {
        processOBDResponse(rxFrame);
        digitalWrite(LED_CAN, HIGH);
        lastCanLedOn = millis();
    }
    if (lastCanLedOn > 0 && millis() - lastCanLedOn > 50) {
        digitalWrite(LED_CAN, LOW);
        lastCanLedOn = 0;
    }
}