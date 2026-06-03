// LTE Version
// =============================================================================

#include <Arduino.h>
#include <ACAN2517FD.h>
#include <SPI.h>
#include <Preferences.h>
#include <esp_sleep.h>

// ==================== SPI (MCP2518FD) ========================================
static const byte MCP2518FD_CS  = 10;
static const byte MCP2518FD_INT = 47;
static const byte SPI_SCK       = 12;
static const byte SPI_MISO      = 13;
static const byte SPI_MOSI      = 11;

// ==================== LED ====================================================
static const byte LED_LTE     = 1;   // LTE/server stav RED
static const byte LED_GPS     = 2;   // GPS hledani / fix BLUE
static const byte LED_CAN     = 3;   // CAN RX activity GREEN

// ==================== A7670E PINS ============================================
#define SIM_RX        18
#define SIM_TX        17
#define SIM_PWRKEY     7
#define SerialAT      Serial1

// ==================== KONFIGURACE SERVERU ====================================
#define API_TOKEN     "SECRET_API_TOKEN"
#define SERVER_URL    "canbussnifferv1-production.up.railway.app"
#define SERVER_PATH   "/data"
#define APN_NAME      "internet"

// ==================== TIMING =================================================
#define OBD_FAST_MS            50   // rpm, speed, throttle, engineLoad: 
#define OBD_SLOW_MS           50   // ostatni PIDy: 
#define LTE_SEND_INTERVAL_MS  2000   // POST kazde 2s
#define GPS_RETRY_INTERVAL_MS 30000
#define CAN_QUIET_TO_IDLE_MS  60000
#define IDLE_TO_SLEEP_MS     300000
#define SLEEP_WAKE_INTERVAL_US (60ULL * 1000000ULL)
#define TRIP_GAP_MS          (60UL * 60 * 1000)

// LED timing
#define LED_LTE_BLINK_MS        80   // delka blinku po uspesnem POST
#define LED_CAN_BLINK_MS        30   // delka blinku po CAN frame
#define LED_GPS_PULSE_MS       100   // delka jednoho pulzu pri hledani fixu
#define LED_GPS_PULSE_GAP_MS   150   // mezera mezi pulzy
#define LED_GPS_CYCLE_GAP_MS  1500   // pauza mezi 3-pulz cykly

// ==================== CAN ====================================================
ACAN2517FD can(MCP2518FD_CS, SPI, MCP2518FD_INT);
void IRAM_ATTR canISR() { can.isr(); }

// ==================== PID DEFINICE ===========================================
enum Category { CAT_CORE, CAT_ENGINE, CAT_FUEL, CAT_EMISSION, CAT_ELECTRIC, CAT_OTHER };

struct OBDPid {
    uint8_t     pid;
    const char* key;
    const char* name;
    const char* unit;
    Category    category;
    bool        fast;        // true = rychla skupina (100ms)
    bool        supported;
    bool        hasValue;
    float       lastValue;
    unsigned long lastRead;
    uint8_t     rawA;
    uint8_t     rawB;
    uint8_t     rawEcuId;
};

OBDPid pidList[] = {
    // ===== RYCHLE (fast=true): rpm, speed, throttle, engineLoad =====
    {0x0C, "rpm",             "RPM",                "rpm",   CAT_CORE,     true,  false, false, 0, 0, 0, 0, 0},
    {0x0D, "speed",           "Speed",              "km/h",  CAT_CORE,     true,  false, false, 0, 0, 0, 0, 0},
    {0x11, "throttle",        "Throttle Pos",       "%",     CAT_CORE,     true,  false, false, 0, 0, 0, 0, 0},
    {0x04, "engineLoad",      "Engine Load",        "%",     CAT_ENGINE,   true,  false, false, 0, 0, 0, 0, 0},

    // ===== POMALE (fast=false) =====
    {0x05, "coolantTemp",     "Coolant Temp",       "C",     CAT_CORE,     false, false, false, 0, 0, 0, 0, 0},
    {0x1F, "engineRuntime",   "Engine Runtime",     "s",     CAT_CORE,     false, false, false, 0, 0, 0, 0, 0},
    {0x06, "stft",            "Short Fuel Trim 1",  "%",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x07, "ltft",            "Long Fuel Trim 1",   "%",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x0B, "mapPressure",     "Intake Manifold Pr", "kPa",   CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x0E, "ignitionTiming",  "Timing Advance",     "deg",   CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x0F, "intakeAirTemp",   "Intake Air Temp",    "C",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x10, "maf",             "MAF Rate",           "g/s",   CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x14, "o2Voltage",       "O2 Sensor 1 V",      "V",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x33, "barometric",      "Barometric Pressure","kPa",   CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x42, "systemVoltage",   "Control Module V",   "V",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x43, "absoluteLoad",    "Absolute Load",      "%",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x46, "outdoorTemp",     "Ambient Air Temp",   "C",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x5C, "oilTemp",         "Engine Oil Temp",    "C",     CAT_ENGINE,   false, false, false, 0, 0, 0, 0, 0},
    {0x0A, "fuelPressure",    "Fuel Pressure",      "kPa",   CAT_FUEL,     false, false, false, 0, 0, 0, 0, 0},
    {0x2F, "fuelLevel",       "Fuel Level",         "%",     CAT_FUEL,     false, false, false, 0, 0, 0, 0, 0},
    {0x5E, "fuelConsumption", "Engine Fuel Rate",   "L/h",   CAT_FUEL,     false, false, false, 0, 0, 0, 0, 0},
    {0x59, "fuelRailPress",   "Fuel Rail Pressure", "kPa",   CAT_FUEL,     false, false, false, 0, 0, 0, 0, 0},
    {0x01, "monitorStatus",   "Monitor Status",     "",      CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x03, "fuelSysStatus",   "Fuel System Status", "",      CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x13, "o2Sensors",       "O2 Sensors Present", "",      CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x1C, "obdStandard",     "OBD Standard",       "",      CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x21, "milDistance",     "Distance w/ MIL",    "km",    CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x30, "warmupsCLR",      "Warmups since CLR",  "",      CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x31, "distanceCLR",     "Distance since CLR", "km",    CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x3C, "catalystTemp",    "Catalyst Temp B1S1", "C",     CAT_EMISSION, false, false, false, 0, 0, 0, 0, 0},
    {0x5B, "hvBatterySOC",    "HV Battery SOC",     "%",     CAT_ELECTRIC, false, false, false, 0, 0, 0, 0, 0},
    {0x9C, "hvBatteryTemp",   "HV Battery Temp",    "C",     CAT_ELECTRIC, false, false, false, 0, 0, 0, 0, 0},
    {0x9D, "hvBatteryVolt",   "HV Battery Voltage", "V",     CAT_ELECTRIC, false, false, false, 0, 0, 0, 0, 0},
    {0x9E, "hvBatteryCurr",   "HV Battery Current", "A",     CAT_ELECTRIC, false, false, false, 0, 0, 0, 0, 0},
    {0x8E, "evMode",          "EV Mode",            "",      CAT_ELECTRIC, false, false, false, 0, 0, 0, 0, 0},
    {0x02, "freezeDTC",       "Freeze DTC",         "",      CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x12, "secAirStatus",    "Sec Air Status",     "",      CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x1D, "o2SensorsAlt",    "O2 Sensors (alt)",   "",      CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x1E, "auxStatus",       "Aux Input Status",   "",      CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x22, "fuelRailPressVac","Fuel Rail Pr vs Vac","kPa",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x23, "fuelRailPressDir","Fuel Rail Pr direct","kPa",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x2C, "egrCmd",          "EGR commanded",      "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x2D, "egrError",        "EGR error",          "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x2E, "evapPurgeCmd",    "Evap Purge",         "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x32, "evapVaporPress",  "Evap Vapor Pr",      "Pa",    CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x44, "afEqRatio",       "Air-Fuel Eq Ratio",  "",      CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x45, "relThrottle",     "Relative Throttle",  "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x47, "absThrottleB",    "Abs Throttle Pos B", "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x49, "accPedalD",       "Accel Pedal D",      "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x4A, "accPedalE",       "Accel Pedal E",      "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x4C, "throttleActuator","Throttle Actuator",  "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x4D, "milTime",         "Time w/ MIL on",     "min",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x4E, "timeSinceCLR",    "Time since CLR",     "min",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x51, "fuelType",        "Fuel Type",          "",      CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x52, "ethanolPct",      "Ethanol %",          "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x53, "evapAbsPress",    "Evap Abs Pr",        "kPa",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x5A, "relAccelPedal",   "Rel Accel Pedal",    "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x5D, "fuelInjTiming",   "Fuel Inj Timing",    "deg",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x61, "demandedTorque",  "Demanded Torque",    "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x62, "actualTorque",    "Actual Torque",      "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x63, "refTorque",       "Reference Torque",   "Nm",    CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x67, "engineCoolantT",  "Engine Coolant Temp","C",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x68, "intakeAirTempS",  "Intake Air Temp Sen","C",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x6B, "egrTemp",         "EGR Temp",           "C",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x70, "boostPressure",   "Boost Pressure",     "kPa",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x71, "chargeAirTemp",   "Charge Air Temp",    "C",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x73, "exhaustPressure", "Exhaust Pressure",   "kPa",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x78, "egtBank1",        "EGT Bank 1",         "C",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x7C, "dpfTemp",         "DPF Temp",           "C",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x83, "noxSensor",       "NOx Sensor",         "ppm",   CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
    {0x9A, "hybridBattLife",  "Hybrid Batt Life",   "%",     CAT_OTHER,    false, false, false, 0, 0, 0, 0, 0},
};

const uint8_t pidCount = sizeof(pidList) / sizeof(pidList[0]);

// ==================== STAVOVE PROMENNE =======================================
bool          scanning           = false;
bool          looping            = false;
unsigned long lastFastRequest    = 0;
unsigned long lastSlowRequest    = 0;
uint8_t       fastIndex          = 0;
uint8_t       slowIndex          = 0;
unsigned long lastLteSend        = 0;
uint8_t       sendCycle          = 0;
unsigned long lastCanFrameMs     = 0;
unsigned long canQuietSinceMs    = 0;
unsigned long lastGpsAttempt     = 0;

bool          lteReady           = false;
bool          gpsEnabled         = false;
bool          gpsHasFix          = false;
double        gpsLat             = 0.0;
double        gpsLon             = 0.0;
uint8_t       httpFailCount      = 0;
unsigned long lastTripActivityMs = 0;
String        nextTripEvent      = "";

// LED non-blocking state
unsigned long ledCanOffMs        = 0;
unsigned long ledLteBlinkOffMs   = 0;
bool          ledLteBlinkActive  = false;
unsigned long ledGpsNextEdgeMs   = 0;
uint8_t       ledGpsPhase        = 0;   // 0..5 = 3 pulzy (on/off/on/off/on/off), 6 = pauza

enum Phase { PHASE_BOOT, PHASE_DRIVING, PHASE_IDLE_AWAKE, PHASE_LIGHT_SLEEP };
Phase phase = PHASE_BOOT;
unsigned long phaseEnteredMs = 0;

Preferences prefs;
RTC_DATA_ATTR uint64_t bootCount = 0;

// =============================================================================
//  FORWARD DECLARATIONS
// =============================================================================

void processOBDResponse(CANFDMessage &frame);
void servCAN();              // <-- non-blocking CAN service, vola se vsude

// =============================================================================
//  CAN SERVICE — vola se behem cekani na AT odpovedi
// =============================================================================

void servCAN() {
    CANFDMessage rx;
    while (can.receive(rx)) {     // while, ne if — vyber vsechny pending framy
        processOBDResponse(rx);
        lastCanFrameMs = millis();
        digitalWrite(LED_CAN, HIGH);
        ledCanOffMs = lastCanFrameMs + LED_CAN_BLINK_MS;
    }
}

// =============================================================================
//  AT HELPERY — neblokuji CAN
// =============================================================================

String sendAT(const String &cmd, uint32_t timeout = 1500, bool log = true) {
    while (SerialAT.available()) SerialAT.read();
    SerialAT.print(cmd); SerialAT.print('\r');
    String resp = "";
    resp.reserve(128);
    unsigned long t = millis();
    while (millis() - t < timeout) {
        if (SerialAT.available()) {
            resp += (char)SerialAT.read();
            // brzo skoncit jakmile mame OK / ERROR
            if (resp.endsWith("OK\r\n") ||
                resp.endsWith("ERROR\r\n") ||
                resp.indexOf("+CME ERROR") >= 0 ||
                resp.indexOf("+CMS ERROR") >= 0) break;
        } else {
            servCAN();              // UART prazdny, cekani na odpoved modemu
                                    // serv.CAN - zatim scanuje CAN
        }
    }
    if (log) {
        Serial.print("[AT] "); Serial.print(cmd); Serial.print(" -> ");
        String r = resp; r.replace("\r", " "); r.replace("\n", " ");
        if (r.length() > 80) r = r.substring(0, 80) + "...";
        Serial.println(r);
    }
    return resp;
}

bool waitFor(const String &substr, uint32_t timeout = 5000) {
    String buf = "";
    buf.reserve(256);
    unsigned long t = millis();
    while (millis() - t < timeout) {
        if (SerialAT.available()) {
            buf += (char)SerialAT.read();
            if (buf.indexOf(substr) >= 0) return true;
        } else {
            servCAN();
        }
    }
    return false;
}

// =============================================================================
//  MODEM POWER
// =============================================================================

void modemPowerPulse() {
    digitalWrite(SIM_PWRKEY, HIGH);
    delay(1500);
    digitalWrite(SIM_PWRKEY, LOW);
    delay(3000);
}

void modemHardReset() {
    Serial.println("[LTE] HARD RESET...");
    digitalWrite(LED_LTE, LOW);
    modemPowerPulse();
    delay(2000);
    modemPowerPulse();
}

// =============================================================================
//  LTE INIT
// =============================================================================

bool initLTE() {
    Serial.println("[LTE] Power ON...");
    modemPowerPulse();
    delay(5000);

    bool alive = false;
    for (uint8_t i = 0; i < 15; i++) {
        String r = sendAT("AT", 1000, false);
        if (r.indexOf("OK") >= 0) { alive = true; break; }
        delay(500);
    }
    if (!alive) { Serial.println("[LTE] Modul nereaguje"); return false; }

    sendAT("ATE0");
    sendAT("AT+CMEE=2");

    Serial.println("[LTE] Cekam na registraci...");
    bool registered = false;
    for (uint8_t i = 0; i < 30; i++) {
        String r = sendAT("AT+CREG?", 1500, false);
        if (r.indexOf(",1") >= 0 || r.indexOf(",5") >= 0) { registered = true; break; }
        delay(1500);
    }
    if (!registered) { Serial.println("[LTE] Registrace selhala"); return false; }

    sendAT("AT+CGDCONT=1,\"IP\",\"" APN_NAME "\"");
    sendAT("AT+CGACT=1,1", 5000);
    sendAT("AT+CSQ");
    sendAT("AT+CSSLCFG=\"sslversion\",0,3");
    sendAT("AT+CSSLCFG=\"authmode\",0,0");
    sendAT("AT+CSSLCFG=\"enableSNI\",0,1");
    sendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1");
    Serial.println("[LTE] OK");
    digitalWrite(LED_LTE, HIGH);   // svit trvale = pripojeno
    return true;
}

// =============================================================================
//  GPS
// =============================================================================

void initGPS() {
    sendAT("AT+CGNSSPWR=1", 3000);
    delay(1500);
    Serial.println("[GPS] Zapnuto, cekam na fix...");
}

void parseGNSSInfo(const String &resp) {
    int idx = resp.indexOf("+CGNSSINFO:");
    if (idx < 0) return;
    String s = resp.substring(idx + 11);
    s.trim();
    if (s.length() < 5) { gpsHasFix = false; return; }

    String parts[16];
    int n = 0, last = 0;
    for (int i = 0; i <= (int)s.length() && n < 16; i++) {
        if (i == (int)s.length() || s.charAt(i) == ',') {
            parts[n++] = s.substring(last, i);
            last = i + 1;
        }
    }
    if (n < 5) { gpsHasFix = false; return; }

    String latStr = parts[1];
    String latNS  = parts[2];
    String lonStr = parts[3];
    String lonEW  = parts[4];

    if (latStr.length() < 4 || lonStr.length() < 4) { gpsHasFix = false; return; }

    gpsLat = latStr.toDouble();
    gpsLon = lonStr.toDouble();
    if (latNS == "S") gpsLat = -gpsLat;
    if (lonEW == "W") gpsLon = -gpsLon;
    gpsHasFix = true;
}

void tryGpsFix() {
    if (!gpsEnabled) return;
    if (millis() - lastGpsAttempt < GPS_RETRY_INTERVAL_MS && lastGpsAttempt != 0) return;
    lastGpsAttempt = millis();
    String r = sendAT("AT+CGNSSINFO", 2000, false);
    bool wasFixed = gpsHasFix;
    parseGNSSInfo(r);
    if (gpsHasFix && !wasFixed) {
        Serial.print("[GPS] FIX lat="); Serial.print(gpsLat, 6);
        Serial.print(" lon="); Serial.println(gpsLon, 6);
    } else if (!gpsHasFix) {
        Serial.println("[GPS] bez fixu");
    }
}

// =============================================================================
//  OBD-II
// =============================================================================

void sendOBDRequest(uint8_t pid) {
    CANFDMessage frame;
    frame.id  = 0x7DF;
    frame.len = 8;
    frame.data[0] = 0x02;
    frame.data[1] = 0x01;
    frame.data[2] = pid;
    memset(&frame.data[3], 0, 5);
    can.tryToSend(frame);
}

float decodePID(uint8_t pid, uint8_t a, uint8_t b) {
    switch (pid) {
        case 0x04: return (a * 100.0f) / 255.0f;
        case 0x05: return a - 40.0f;
        case 0x06: return (a - 128.0f) * 100.0f / 128.0f;
        case 0x07: return (a - 128.0f) * 100.0f / 128.0f;
        case 0x0A: return a * 3.0f;
        case 0x0B: return a;
        case 0x0C: return ((a * 256.0f) + b) / 4.0f;
        case 0x0D: return a;
        case 0x0E: return (a / 2.0f) - 64.0f;
        case 0x0F: return a - 40.0f;
        case 0x10: return ((a * 256.0f) + b) / 100.0f;
        case 0x11: return (a * 100.0f) / 255.0f;
        case 0x14: return a / 200.0f;
        case 0x1F: return (a * 256.0f) + b;
        case 0x21: return (a * 256.0f) + b;
        case 0x2F: return (a * 100.0f) / 255.0f;
        case 0x31: return (a * 256.0f) + b;
        case 0x33: return a;
        case 0x3C: return ((a * 256.0f) + b) / 10.0f - 40.0f;
        case 0x42: return ((a * 256.0f) + b) / 1000.0f;
        case 0x43: return ((a * 256.0f) + b) * 100.0f / 255.0f;
        case 0x46: return a - 40.0f;
        case 0x4D: return (a * 256.0f) + b;
        case 0x5B: return (a * 100.0f) / 255.0f;
        case 0x5C: return a - 40.0f;
        case 0x5E: return ((a * 256.0f) + b) / 20.0f;
        case 0x59: return ((a * 256.0f) + b) * 10.0f;
        case 0x9C: return a - 40.0f;
        case 0x9D: return ((a * 256.0f) + b) / 10.0f;
        case 0x9E: return ((a * 256.0f) + b) / 10.0f - 3276.8f;
        default:   return a;
    }
}

void processSupportedBitmask(uint8_t basePid, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    uint32_t bm = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
    for (uint8_t i = 0; i < 32; i++) {
        if (bm & (1UL << (31 - i))) {
            uint8_t sp = basePid + i + 1;
            for (uint8_t j = 0; j < pidCount; j++) {
                if (pidList[j].pid == sp) pidList[j].supported = true;
            }
        }
    }
}

void processOBDResponse(CANFDMessage &frame) {
    if (frame.id < 0x7E8 || frame.id > 0x7EF) return;
    if (frame.data[1] != 0x41) return;

    uint8_t pid = frame.data[2];

    if (pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60 || pid == 0x80) {
        processSupportedBitmask(pid, frame.data[3], frame.data[4], frame.data[5], frame.data[6]);
        if (scanning) {
            if      (pid == 0x00) sendOBDRequest(0x20);
            else if (pid == 0x20) sendOBDRequest(0x40);
            else if (pid == 0x40) sendOBDRequest(0x60);
            else if (pid == 0x60) sendOBDRequest(0x80);
            else {
                scanning = false;
                uint8_t cnt = 0;
                for (uint8_t i = 0; i < pidCount; i++) if (pidList[i].supported) cnt++;
                Serial.print("[OBD] SCAN OK, podporovanych PIDu: "); Serial.println(cnt);
                looping = true;
                fastIndex = 0; slowIndex = 0;
                lastFastRequest = 0; lastSlowRequest = 0;
            }
        }
        return;
    }

    float v = decodePID(pid, frame.data[3], frame.data[4]);
    for (uint8_t i = 0; i < pidCount; i++) {
        if (pidList[i].pid == pid) {
            pidList[i].lastValue = v;
            pidList[i].lastRead  = millis();
            pidList[i].hasValue  = true;
            pidList[i].rawA      = frame.data[3];
            pidList[i].rawB      = frame.data[4];
            pidList[i].rawEcuId  = frame.id & 0x0F;
            break;
        }
    }
}

int8_t nextPid(uint8_t &idx, bool wantFast) {
    for (uint8_t tries = 0; tries < pidCount; tries++) {
        uint8_t i = idx % pidCount;
        idx++;
        if (pidList[i].supported && pidList[i].fast == wantFast) return i;
    }
    return -1;
}

// =============================================================================
//  NONCE
// =============================================================================

void initNonce() {
    prefs.begin("obd", false);
    bootCount = prefs.getULong64("boot", 0);
    bootCount++;
    prefs.putULong64("boot", bootCount);
    prefs.end();
    Serial.print("[NONCE] bootCount = "); Serial.println((unsigned long)bootCount);
}

uint64_t makeNonce() {
    return (bootCount * 1000000000ULL) + (uint64_t)millis();
}

// =============================================================================
//  JSON PAYLOAD — pres reserve() + char buffer
// =============================================================================

String buildFastJSON() {
    String j;
    j.reserve(256);
    j = "{\"nonce\":";
    j += String((unsigned long long)makeNonce());
    j += ",\"type\":\"fast\"";
    char kv[48];
    for (uint8_t i = 0; i < pidCount; i++) {
        if (!pidList[i].supported || !pidList[i].fast || !pidList[i].hasValue) continue;
        snprintf(kv, sizeof(kv), ",\"%s\":%.1f", pidList[i].key, pidList[i].lastValue);
        j += kv;
    }
    j += "}";
    return j;
}

String buildFullJSON() {
    String j;
    j.reserve(1024);
    j = "{\"nonce\":";
    j += String((unsigned long long)makeNonce());
    j += ",\"type\":\"full\"";
    if (gpsHasFix) {
        char buf[64];
        snprintf(buf, sizeof(buf), ",\"lat\":%.6f,\"lon\":%.6f", gpsLat, gpsLon);
        j += buf;
    }
    if (nextTripEvent.length() > 0) {
        j += ",\"event\":\"" + nextTripEvent + "\"";
    }
    char kv[48];
    for (uint8_t i = 0; i < pidCount; i++) {
    if (!pidList[i].supported || !pidList[i].hasValue) continue;
    const char* key = pidList[i].key;
    if (key == nullptr || key[0] == '\0') {
        char fallback[12];
        snprintf(fallback, sizeof(fallback), "pid_%02X", pidList[i].pid);
        snprintf(kv, sizeof(kv), ",\"%s\":%.1f", fallback, pidList[i].lastValue);
    } else {
        snprintf(kv, sizeof(kv), ",\"%s\":%.1f", key, pidList[i].lastValue);
    }
    j += kv;
    }
    j += "}";
    return j;
}

// =============================================================================
//  HTTPS POST
// =============================================================================

bool sendToServer(const String &payload) {
    Serial.print("[HTTP] payload ("); Serial.print(payload.length()); Serial.println(" B)");

    sendAT("AT+HTTPTERM", 1000, false);
    delay(200);
    sendAT("AT+HTTPINIT");
    sendAT("AT+HTTPPARA=\"CID\",1");
    sendAT("AT+HTTPPARA=\"URL\",\"https://" SERVER_URL SERVER_PATH "\"");
    sendAT("AT+HTTPPARA=\"CONTENT\",\"application/json\"");
    sendAT("AT+HTTPPARA=\"USERDATA\",\"Authorization: Bearer " API_TOKEN "\"");
    sendAT("AT+HTTPPARA=\"SSLCFG\",0");

    String dlCmd = "AT+HTTPDATA=" + String(payload.length()) + ",10000";
    SerialAT.print(dlCmd); SerialAT.print('\r');
    if (!waitFor("DOWNLOAD", 5000)) {
        Serial.println("[HTTP] DOWNLOAD timeout");
        sendAT("AT+HTTPTERM", 1000, false);
        httpFailCount++;
        if (httpFailCount >= 3) {
            Serial.println("[HTTP] 3x selhani -> modem reset");
            modemHardReset();
            lteReady = initLTE();
            if (lteReady) { initGPS(); gpsEnabled = true; }
            httpFailCount = 0;
        }
        return false;
    }
    SerialAT.print(payload);
    if (!waitFor("OK", 5000)) {
        Serial.println("[HTTP] upload selhal");
        sendAT("AT+HTTPTERM", 1000, false);
        return false;
    }
    httpFailCount = 0;

    SerialAT.print("AT+HTTPACTION=1\r");
    String resp = "";
    resp.reserve(256);
    unsigned long t = millis();
    while (millis() - t < 15000) {
        if (SerialAT.available()) {
            resp += (char)SerialAT.read();
            if (resp.indexOf("+HTTPACTION:") >= 0 && resp.indexOf("\n") >= 0) break;
        } else {
            servCAN();  
        }
    }

    String body = sendAT("AT+HTTPREAD=0,512", 3000, false);
    sendAT("AT+HTTPTERM", 1000, false);

    if (resp.indexOf("+HTTPACTION: 1,200") >= 0) {
        Serial.println("[HTTP] 200 OK");
        // bliknuti LTE LED — non-blocking, vrati se po LED_LTE_BLINK_MS do trvaleho stavu
        digitalWrite(LED_LTE, LOW);
        ledLteBlinkActive = true;
        ledLteBlinkOffMs  = millis() + LED_LTE_BLINK_MS;

        int ci = body.indexOf("\"cmd\":\"");
        if (ci >= 0) {
            int s = ci + 7, e = body.indexOf("\"", s);
            if (e > s) {
                String cmd = body.substring(s, e);
                cmd.toUpperCase();
                if (cmd == "SCAN") {
                    scanning = true; looping = false;
                    for (uint8_t i = 0; i < pidCount; i++) {
                        pidList[i].supported = false;
                        pidList[i].hasValue  = false;
                    }
                    sendOBDRequest(0x00);
                } else if (cmd == "LOOP") {
                    looping = true; fastIndex = 0; slowIndex = 0;
                } else if (cmd == "STOP") {
                    looping = false;
                }
            }
        }
        nextTripEvent = "";
        return true;
    }

    if      (resp.indexOf("+HTTPACTION: 1,401") >= 0) Serial.println("[HTTP] 401 spatny token");
    else if (resp.indexOf("+HTTPACTION: 1,400") >= 0) Serial.println("[HTTP] 400 replay/format");
    else { Serial.print("[HTTP] chyba: "); Serial.println(resp.substring(0, 60)); }
    return false;
}

// =============================================================================
//  TRIP + FSM
// =============================================================================

void onEnterDriving() {
    Serial.println("[TRIP] motor bezi");
    unsigned long now = millis();
    bool firstTrip = (lastTripActivityMs == 0);
    unsigned long gap = firstTrip ? 0 : (now - lastTripActivityMs);
    nextTripEvent = (firstTrip || gap > TRIP_GAP_MS) ? "trip_start" : "trip_continue";
    Serial.print("[TRIP] "); Serial.println(nextTripEvent);
    lastTripActivityMs = now;
    lastLteSend = 0;
    looping = true; fastIndex = 0; slowIndex = 0;
    lastFastRequest = 0; lastSlowRequest = 0;
}

void onEnterIdleAwake() {
    Serial.println("[TRIP] CAN ticho -> IDLE");
    looping = false;
    lastTripActivityMs = millis();
}

void enterLightSleep() {
    Serial.println("[SLEEP] Uspinam...");
    nextTripEvent = "trip_end";
    if (lteReady) sendToServer(buildFullJSON());
    sendAT("AT+CSCLK=1", 1000);
    digitalWrite(LED_LTE, LOW);
    digitalWrite(LED_GPS, LOW);
    digitalWrite(LED_CAN, LOW);
    esp_sleep_enable_timer_wakeup(SLEEP_WAKE_INTERVAL_US);
    Serial.flush();
    esp_light_sleep_start();
    Serial.println("[SLEEP] Wake check");
    sendAT("AT+CSCLK=0", 1000);
    if (lteReady) digitalWrite(LED_LTE, HIGH);
}

void updatePhase() {
    unsigned long now = millis();
    switch (phase) {
        case PHASE_BOOT:
            if (lastCanFrameMs > 0) {
                phase = PHASE_DRIVING; phaseEnteredMs = now;
                onEnterDriving();
            }
            break;
        case PHASE_DRIVING:
            if (lastCanFrameMs > 0 && now - lastCanFrameMs > CAN_QUIET_TO_IDLE_MS) {
                phase = PHASE_IDLE_AWAKE; phaseEnteredMs = now;
                canQuietSinceMs = lastCanFrameMs;
                onEnterIdleAwake();
            }
            break;
        case PHASE_IDLE_AWAKE:
            if (lastCanFrameMs > canQuietSinceMs) {
                phase = PHASE_DRIVING; phaseEnteredMs = now;
                onEnterDriving();
                break;
            }
            if (now - phaseEnteredMs > IDLE_TO_SLEEP_MS) {
                phase = PHASE_LIGHT_SLEEP; phaseEnteredMs = now;
                enterLightSleep();
                phase = PHASE_IDLE_AWAKE; phaseEnteredMs = now;
                lastCanFrameMs = 0;
            }
            break;
        case PHASE_LIGHT_SLEEP:
            break;
    }
}

// =============================================================================
//  LED MANAGER — non-blocking, vola se v loop()
// =============================================================================

void updateLEDs() {
    unsigned long now = millis();

    // ---- LED1 (LTE): trvaly stav z lteReady, kratky off-blink po POSTu ----
    if (ledLteBlinkActive) {
        if (now >= ledLteBlinkOffMs) {
            ledLteBlinkActive = false;
            digitalWrite(LED_LTE, lteReady ? HIGH : LOW);
        }
    } else {
        // pasivni stav: sleduj lteReady
        digitalWrite(LED_LTE, lteReady ? HIGH : LOW);
    }

    // ---- LED2 (GPS): fix=svit, jinak 3 pulzy + pauza ----
    if (gpsHasFix) {
        digitalWrite(LED_GPS, HIGH);
        ledGpsPhase = 0;
        ledGpsNextEdgeMs = 0;
    } else {
        // 6 hran (3 pulzy on/off) + 1 dlouha pauza
        if (now >= ledGpsNextEdgeMs) {
            switch (ledGpsPhase) {
                case 0: digitalWrite(LED_GPS, HIGH); ledGpsNextEdgeMs = now + LED_GPS_PULSE_MS;     break;
                case 1: digitalWrite(LED_GPS, LOW);  ledGpsNextEdgeMs = now + LED_GPS_PULSE_GAP_MS; break;
                case 2: digitalWrite(LED_GPS, HIGH); ledGpsNextEdgeMs = now + LED_GPS_PULSE_MS;     break;
                case 3: digitalWrite(LED_GPS, LOW);  ledGpsNextEdgeMs = now + LED_GPS_PULSE_GAP_MS; break;
                case 4: digitalWrite(LED_GPS, HIGH); ledGpsNextEdgeMs = now + LED_GPS_PULSE_MS;     break;
                case 5: digitalWrite(LED_GPS, LOW);  ledGpsNextEdgeMs = now + LED_GPS_CYCLE_GAP_MS; break;
            }
            ledGpsPhase = (ledGpsPhase + 1) % 6;
        }
    }

    // ---- LED3 (CAN): non-blocking timeout pro short blink ----
    if (ledCanOffMs && now >= ledCanOffMs) {
        digitalWrite(LED_CAN, LOW);
        ledCanOffMs = 0;
    }
}

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n==================================");
    Serial.println("  OBD-II LTE Tracker  v1.1 (fast)");
    Serial.println("  ESP32-S3 + MCP2518FD + A7670E");
    Serial.println("==================================");

    pinMode(LED_LTE, OUTPUT); digitalWrite(LED_LTE, LOW);
    pinMode(LED_GPS, OUTPUT); digitalWrite(LED_GPS, LOW);
    pinMode(LED_CAN, OUTPUT); digitalWrite(LED_CAN, LOW);
    pinMode(SIM_PWRKEY, OUTPUT); digitalWrite(SIM_PWRKEY, LOW);

    initNonce();

    // 1. CAN init — musi byt prvni
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    ACAN2517FDSettings canSettings(ACAN2517FDSettings::OSC_40MHz, 500 * 1000, DataBitRateFactor::x1);
    canSettings.mRequestedMode = ACAN2517FDSettings::Normal20B;
    const uint32_t canErr = can.begin(canSettings, canISR);
    if (canErr == 0) {
        Serial.println("[CAN] OK @ 500 kbit/s");

    } else {
        Serial.print("[CAN] CHYBA: 0x"); Serial.println(canErr, HEX);
    }

    // 2. SCAN hned pred LTE initem (max 10s)
    Serial.println("[OBD] SCAN spousti...");
    scanning = true;
    sendOBDRequest(0x00);
    {
        unsigned long deadline = millis() + 10000;
        while (millis() < deadline && scanning) {
            servCAN();
            updateLEDs();
            delay(2);
        }
        if (!scanning) {
            uint8_t cnt = 0;
            for (uint8_t i = 0; i < pidCount; i++) if (pidList[i].supported) cnt++;
            Serial.print("[OBD] SCAN pred LTE OK, PIDu: "); Serial.println(cnt);
        } else {
            Serial.println("[OBD] SCAN timeout, pokracuje v loop()");
        }
    }

    // 3. LTE + GPS
    SerialAT.begin(115200, SERIAL_8N1, SIM_RX, SIM_TX);
    delay(1000);
    lteReady = initLTE();
    if (lteReady) {
        initGPS();
        gpsEnabled = true;
    }
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
    unsigned long now = millis();

    // ---- CAN RX (servCAN servuje vsechny pending framy + LED) ----
    servCAN();

    // Auto-SCAN pokud prijde frame ale zadne PIDy supported
    if (lastCanFrameMs == now && !scanning && !looping) {
        uint8_t cnt = 0;
        for (uint8_t i = 0; i < pidCount; i++) if (pidList[i].supported) cnt++;
        if (cnt == 0) {
            Serial.println("[OBD] Auto-SCAN...");
            scanning = true;
            sendOBDRequest(0x00);
        }
    }

    // ---- RYCHLE PIDy (rpm, speed, throttle, engineLoad) — kazdych 50 sms ----
    if (looping && now - lastFastRequest >= OBD_FAST_MS) {
        lastFastRequest = now;
        int8_t idx = nextPid(fastIndex, true);
        if (idx >= 0) sendOBDRequest(pidList[idx].pid);
    }

    // ---- POMALE PIDy — 50 ms ----
    if (looping && now - lastSlowRequest >= OBD_SLOW_MS) {
        lastSlowRequest = now;
        int8_t idx = nextPid(slowIndex, false);
        if (idx >= 0) sendOBDRequest(pidList[idx].pid);
    }

    // ---- GPS retry ----
    if (lteReady) tryGpsFix();

    // ---- LTE send: kazdych 2s fast JSON, kazdych ~6s full JSON ----
    if (lteReady && phase != PHASE_IDLE_AWAKE && now - lastLteSend >= LTE_SEND_INTERVAL_MS) {
        lastLteSend = now;
        sendCycle++;
        if (sendCycle >= 3) {
            sendCycle = 0;
            sendToServer(buildFullJSON());
        } else {
            sendToServer(buildFastJSON());
        }
    }

    // ---- LTE reconnect pokud nikdy nepripojeno (napr. v aute zapneme drive nez prijde signal) ----
    static unsigned long lastLteRetry = 0;
    if (!lteReady && now - lastLteRetry > 60000) {
        lastLteRetry = now;
        Serial.println("[LTE] Pokus o opetovne pripojeni...");
        lteReady = initLTE();
        if (lteReady && !gpsEnabled) { initGPS(); gpsEnabled = true; }
    }

    // ---- FSM (sleep mgmt) ----
    updatePhase();

    // ---- LED update (non-blocking) ----
    updateLEDs();
}