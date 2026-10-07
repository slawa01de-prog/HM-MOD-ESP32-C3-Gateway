
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <SPIFFS.h>
#include <Update.h>
#include <esp_system.h>

// ============================================================
// HM-MOD-RPI-PCB + ESP32-C3
// HB-RF-ETH network dongle for OpenCCU / RaspberryMatic
// Version 3.1 Pro: DualCoPro-aware + diagnostics + ESP OTA + HM updater
//
// Wiring:
// HM pin 10 TX  -> ESP GPIO6 RX
// HM pin 8  RX  <- ESP GPIO5 TX
// HM pin 12 RST <- ESP GPIO7
// HM pin 1 3V3  -> ESP 3V3
// HM GND        -> ESP GND
// ============================================================

#define HM_RX_PIN       6
#define HM_TX_PIN       5
#define HM_RESET_PIN    7
#define HM_BAUD         115200

#define HB_PORT         3008
#define HTTP_PORT       80
#define MAX_HB_CLIENTS  4

#define T_CONNECT       0
#define T_DISCONNECT    1
#define T_KEEPALIVE     2
#define T_LED           3
#define T_RESET         4
#define T_STARTCONN     5
#define T_STOPCONN      6
#define T_FRAME         7

#define HM_DST_HMSYSTEM 0x00
#define HM_DST_TRX      0x01
#define HM_DST_HMIP     0x02
#define HM_DST_LLMAC    0x03
#define HM_DST_COMMON   0xFE
#define HM_DST_OS       HM_DST_HMSYSTEM

#define HM_CMD_HMSYSTEM_IDENTIFY     0x00
#define HM_CMD_HMSYSTEM_GET_VERSION  0x02
#define HM_CMD_HMSYSTEM_CHANGE_APP   0x03
#define HM_CMD_HMSYSTEM_ACK          0x04
#define HM_CMD_HMSYSTEM_GET_SERIAL   0x0B
#define HM_CMD_UPDATE_FW             0x05

#define HM_CMD_TRX_GET_VERSION       0x02
#define HM_CMD_TRX_ACK               0x04
#define HM_CMD_TRX_GET_MCU_TYPE      0x09
#define HM_CMD_TRX_GET_DEFAULT_RF_ADDR 0x10

#define HM_CMD_HMIP_GET_DEFAULT_RF_ADDR 0x01
#define HM_CMD_HMIP_ACK              0x06

#define HM_CMD_LLMAC_ACK             0x01
#define HM_CMD_LLMAC_GET_SERIAL      0x07
#define HM_CMD_LLMAC_GET_DEFAULT_RF_ADDR 0x08

#define HM_CMD_COMMON_IDENTIFY       0x01
#define HM_CMD_COMMON_START_BL       0x02
#define HM_CMD_COMMON_START_APP      0x03
#define HM_CMD_COMMON_GET_SGTIN      0x04
#define HM_CMD_COMMON_ACK            0x05

#define FW_PATH         "/hmfw.eq3"
#define MAX_FW_BLOCK    2048

HardwareSerial HM(1);
WiFiUDP hbUdp;
WebServer web(HTTP_PORT);
Preferences prefs;

struct HbClient {
  bool connected = false;
  bool started = false;
  IPAddress ip;
  uint16_t port = 0;
  uint8_t endpointId = 1;
  uint8_t txCounter = 0;
  uint32_t lastRxMs = 0;
};

HbClient clients[MAX_HB_CLIENTS];

String wifiSsid;
String wifiPass;
String hostName;

String moduleTag = "unknown";
String moduleFirmware = "unknown";
String moduleSerial = "unknown";
String moduleSGTIN = "unknown";
String moduleTypeName = "HM-MOD-RPI-PCB";
uint8_t moduleMcuType = 0;
uint32_t moduleHmIpAddress = 0;
uint32_t moduleBidCosAddress = 0;
String lastHostFrame = "";
String lastHmFrame = "";
uint32_t lastHmFrameMs = 0;
uint32_t lastHostFrameMs = 0;

uint32_t statConnects = 0;
uint32_t statDisconnects = 0;
uint32_t statBadCrc = 0;
uint32_t statUnknown = 0;
uint32_t statHostToHm = 0;
uint32_t statHmToHost = 0;
uint32_t statResets = 0;
uint32_t statKeepaliveTimeout = 0;
uint32_t statUartFrames = 0;
uint32_t statUartFrameDrops = 0;
uint32_t statResetOpenCCU = 0;
uint32_t statResetWeb = 0;
uint32_t statResetStartup = 0;
uint32_t statResetUpdater = 0;
uint32_t statWifiReconnects = 0;
uint32_t lastWifiOkMs = 0;

#define SYSLOG_LINES 48
#define SYSLOG_WIDTH 112
char sysLog[SYSLOG_LINES][SYSLOG_WIDTH];
uint8_t sysLogHead = 0;
uint8_t sysLogCount = 0;

volatile bool maintenanceMode = false;
volatile bool flashBusy = false;
bool fwUploaded = false;
size_t fwUploadedBytes = 0;
int fwBlockCount = 0;
int flashPercent = 0;
String flashStatus = "Bereit";
String flashError = "";
bool rebootPending = false;
uint32_t rebootAt = 0;

// ============================================================
// CRC16 - poly 0x8005, init 0xD77F
// ============================================================
uint16_t crc16Bidcos(const uint8_t *data, size_t len) {
  uint16_t crc = 0xD77F;
  while (len--) {
    crc ^= ((uint16_t)(*data++)) << 8;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x8005);
      else crc = (uint16_t)(crc << 1);
    }
  }
  return crc;
}

String hexShort(const uint8_t *data, size_t len, size_t maxBytes = 64) {
  String s;
  size_t n = min(len, maxBytes);
  s.reserve(n * 3 + 8);
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X", data[i]);
    if (i) s += ' ';
    s += b;
  }
  if (len > n) s += " ...";
  return s;
}


String formatUptime() {
  uint32_t sec = millis() / 1000;
  uint32_t days = sec / 86400; sec %= 86400;
  uint32_t hrs = sec / 3600; sec %= 3600;
  uint32_t min = sec / 60; sec %= 60;
  char b[48];
  snprintf(b, sizeof(b), "%ud %02u:%02u:%02u", days, hrs, min, sec);
  return String(b);
}

String resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_SW: return "Software";
    case ESP_RST_PANIC: return "Panic";
    case ESP_RST_INT_WDT: return "Interrupt-WDT";
    case ESP_RST_TASK_WDT: return "Task-WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_DEEPSLEEP: return "DeepSleep";
    default: return "Andere";
  }
}

void addLog(const String &msg) {
  char line[SYSLOG_WIDTH];
  snprintf(line, sizeof(line), "[%10lu ms] %s", (unsigned long)millis(), msg.c_str());
  strncpy(sysLog[sysLogHead], line, SYSLOG_WIDTH - 1);
  sysLog[sysLogHead][SYSLOG_WIDTH - 1] = 0;
  sysLogHead = (sysLogHead + 1) % SYSLOG_LINES;
  if (sysLogCount < SYSLOG_LINES) sysLogCount++;
  Serial.println(line);
}

String htmlEscape(String v) {
  v.replace("&", "&amp;");
  v.replace("<", "&lt;");
  v.replace(">", "&gt;");
  v.replace("\"", "&quot;");
  return v;
}

String hexAddress24(uint32_t a) {
  if (!a) return "unknown";
  char b[16];
  snprintf(b, sizeof(b), "0x%06lX", (unsigned long)(a & 0xFFFFFF));
  return String(b);
}

// ============================================================
// HM reset
// active LOW
// ============================================================
void hmResetPulse(const char *source = "Local") {
  pinMode(HM_RESET_PIN, OUTPUT);
  digitalWrite(HM_RESET_PIN, LOW);
  delay(50);
  digitalWrite(HM_RESET_PIN, HIGH);
  delay(50);
  statResets++;
  if (!strcmp(source, "OpenCCU")) statResetOpenCCU++;
  else if (!strcmp(source, "Web")) statResetWeb++;
  else if (!strcmp(source, "Startup")) statResetStartup++;
  else if (!strcmp(source, "Updater")) statResetUpdater++;
  moduleTag = "Co_CPU_BL";
  addLog(String("HM reset: ") + source);
}

// ============================================================
// HM wire parser for normal HB-RF-ETH operation.
// Keeps encoded frame bytes exactly as sent by module.
// ============================================================
class HmWireParser {
public:
  static const size_t CAP = 2048;

  enum State {
    NO_DATA,
    LEN_HI,
    LEN_LO,
    FRAME_DATA
  };

  uint8_t buf[CAP];
  size_t pos = 0;
  State state = NO_DATA;
  bool escaped = false;
  uint16_t frameLen = 0;
  uint16_t framePos = 0;

  void reset() {
    pos = 0;
    state = NO_DATA;
    escaped = false;
    frameLen = 0;
    framePos = 0;
  }

  bool push(uint8_t chr) {
    if (chr == 0xFD) {
      pos = 0;
      escaped = false;
      state = LEN_HI;
      frameLen = 0;
      framePos = 0;
      buf[pos++] = chr;
      return false;
    }

    if (state == NO_DATA) return false;

    if (pos >= CAP) {
      reset();
      statUartFrameDrops++;
      return false;
    }

    buf[pos++] = chr;

    if (chr == 0xFC && !escaped) {
      escaped = true;
      return false;
    }

    uint8_t decoded = chr;
    if (escaped) {
      decoded = chr | 0x80;
      escaped = false;
    }

    switch (state) {
      case LEN_HI:
        frameLen = ((uint16_t)decoded) << 8;
        state = LEN_LO;
        break;

      case LEN_LO:
        frameLen |= decoded;
        frameLen += 2; // CRC
        framePos = 0;
        if (frameLen > 1900) {
          reset();
          statUartFrameDrops++;
        } else {
          state = FRAME_DATA;
        }
        break;

      case FRAME_DATA:
        framePos++;
        if (framePos >= frameLen) {
          state = NO_DATA;
          return true;
        }
        break;

      default:
        break;
    }

    return false;
  }
};

HmWireParser hmParser;

// ============================================================
// Low-level decoded frame reader for updater.
// Output buffer contains unescaped bytes including:
// FD lenHi lenLo dst cnt payload... crcHi crcLo
// ============================================================
bool readHmDecodedFrame(uint8_t *out, size_t cap, size_t &outLen, uint32_t timeoutMs) {
  outLen = 0;
  bool started = false;
  bool escaped = false;
  size_t totalNeed = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    while (HM.available()) {
      uint8_t b = (uint8_t)HM.read();

      if (!started) {
        if (b != 0xFD) continue;
        started = true;
        outLen = 0;
        if (cap < 1) return false;
        out[outLen++] = 0xFD;
        escaped = false;
        continue;
      }

      if (b == 0xFD && !escaped) {
        // New frame start; resync.
        outLen = 0;
        out[outLen++] = 0xFD;
        totalNeed = 0;
        escaped = false;
        continue;
      }

      if (b == 0xFC && !escaped) {
        escaped = true;
        continue;
      }

      if (escaped) {
        b |= 0x80;
        escaped = false;
      }

      if (outLen >= cap) return false;
      out[outLen++] = b;

      if (outLen == 3) {
        uint16_t packetLen = ((uint16_t)out[1] << 8) | out[2];
        totalNeed = (size_t)packetLen + 5;
        if (totalNeed > cap || packetLen > 3000) return false;
      }

      if (totalNeed && outLen >= totalNeed) {
        uint16_t calc = crc16Bidcos(out, outLen - 2);
        uint16_t got = ((uint16_t)out[outLen - 2] << 8) | out[outLen - 1];
        return calc == got;
      }
    }

    delay(1);
    yield();
  }

  return false;
}

void hmSendEscaped(uint8_t b) {
  if (b == 0xFC || b == 0xFD) {
    HM.write(0xFC);
    HM.write(b & 0x7F);
  } else {
    HM.write(b);
  }
}

void hmSendCommand(uint8_t dst, uint8_t cnt, const uint8_t *payload, size_t plen) {
  uint16_t packetLen = (uint16_t)(plen + 2);

  uint8_t head[5];
  head[0] = 0xFD;
  head[1] = (uint8_t)(packetLen >> 8);
  head[2] = (uint8_t)(packetLen & 0xFF);
  head[3] = dst;
  head[4] = cnt;

  // CRC over unescaped frame without CRC bytes.
  uint8_t raw[2100];
  if (plen + 5 > sizeof(raw)) return;

  memcpy(raw, head, 5);
  if (plen) memcpy(raw + 5, payload, plen);
  uint16_t crc = crc16Bidcos(raw, plen + 5);

  HM.write(0xFD);
  for (int i = 1; i < 5; i++) hmSendEscaped(head[i]);
  for (size_t i = 0; i < plen; i++) hmSendEscaped(payload[i]);
  hmSendEscaped((uint8_t)(crc >> 8));
  hmSendEscaped((uint8_t)(crc & 0xFF));
  HM.flush();
}

bool frameHasAscii(const uint8_t *frame, size_t len, const char *needle) {
  size_t nl = strlen(needle);
  if (nl == 0 || len < nl) return false;

  for (size_t i = 0; i + nl <= len; i++) {
    bool ok = true;
    for (size_t j = 0; j < nl; j++) {
      if (frame[i + j] != (uint8_t)needle[j]) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
  }
  return false;
}

bool detectModeTagInFrame(const uint8_t *frame, size_t len, String &tag) {
  const char *tags[] = {"DualCoPro_App", "HMIP_TRX_App", "HMIP_TRX_Bl", "Co_CPU_App", "Co_CPU_BL"};
  for (const char *t : tags) {
    if (frameHasAscii(frame, len, t)) {
      tag = t;
      return true;
    }
  }
  return false;
}

bool waitForBootTag(String &tag, uint32_t timeoutMs) {
  uint8_t frame[2300];
  size_t len = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    uint32_t remain = timeoutMs - (millis() - start);
    if (!readHmDecodedFrame(frame, sizeof(frame), len, min((uint32_t)500, remain))) continue;
    if (detectModeTagInFrame(frame, len, tag)) return true;
  }
  return false;
}

bool isBootMode(const String &tag) {
  return tag == "Co_CPU_BL" || tag == "HMIP_TRX_Bl";
}

bool isAppMode(const String &tag) {
  return tag == "Co_CPU_App" || tag == "DualCoPro_App" || tag == "HMIP_TRX_App";
}

bool identifyMode(String &tag, uint32_t timeoutMs = 1800) {
  uint8_t frame[2300];
  size_t len = 0;
  uint8_t cnt = 0x60;

  while (HM.available()) HM.read();

  uint8_t cmd = HM_CMD_COMMON_IDENTIFY;
  hmSendCommand(HM_DST_COMMON, cnt++, &cmd, 1);
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, sizeof(frame), len, 500)) continue;
    if (detectModeTagInFrame(frame, len, tag)) return true;
  }

  cmd = HM_CMD_HMSYSTEM_IDENTIFY;
  hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  start = millis();
  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, sizeof(frame), len, 500)) continue;
    if (detectModeTagInFrame(frame, len, tag)) return true;
  }

  return false;
}

bool enterBootloader() {
  while (HM.available()) HM.read();
  hmResetPulse("Updater");

  String tag;
  if (waitForBootTag(tag, 2200)) {
    moduleTag = tag;
    if (isBootMode(tag)) return true;
  }

  if (!identifyMode(tag, 1500)) return false;
  moduleTag = tag;
  if (isBootMode(tag)) return true;

  uint8_t frame[2300];
  size_t len = 0;
  uint8_t cnt = 0x70;
  uint8_t cmd;

  if (tag == "DualCoPro_App" || tag == "HMIP_TRX_App") {
    cmd = HM_CMD_COMMON_START_BL;
    hmSendCommand(HM_DST_COMMON, cnt++, &cmd, 1);
  } else if (tag == "Co_CPU_App") {
    cmd = HM_CMD_HMSYSTEM_CHANGE_APP;
    hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  } else {
    return false;
  }

  readHmDecodedFrame(frame, sizeof(frame), len, 1200);
  if (waitForBootTag(tag, 3000) && isBootMode(tag)) {
    moduleTag = tag;
    delay(700);
    return true;
  }
  return false;
}

bool enterApplication() {
  String tag;
  if (identifyMode(tag, 1500)) {
    moduleTag = tag;
    if (isAppMode(tag)) return true;
  } else {
    // On a freshly reset module the unsolicited boot banner may be the only clue.
    if (waitForBootTag(tag, 1000)) {
      moduleTag = tag;
      if (isAppMode(tag)) return true;
    }
  }

  uint8_t frame[2300];
  size_t len = 0;
  uint8_t cnt = 0x80;
  uint8_t cmd;

  // HM-MOD-RPI-PCB bootloader identifies as Co_CPU_BL even when the
  // installed application is the 2.8.6 DualCoPro firmware.
  if (moduleTag == "Co_CPU_BL") {
    cmd = HM_CMD_HMSYSTEM_CHANGE_APP;
    hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  } else if (moduleTag == "HMIP_TRX_Bl") {
    cmd = HM_CMD_COMMON_START_APP;
    hmSendCommand(HM_DST_COMMON, cnt++, &cmd, 1);
  } else {
    // Try legacy start command as last resort for HM-MOD-RPI-PCB.
    cmd = HM_CMD_HMSYSTEM_CHANGE_APP;
    hmSendCommand(HM_DST_HMSYSTEM, cnt++, &cmd, 1);
  }

  readHmDecodedFrame(frame, sizeof(frame), len, 1200); // usually ACK

  if (waitForBootTag(tag, 3500) && isAppMode(tag)) {
    moduleTag = tag;
    addLog("HM application: " + tag);
    delay(400);
    return true;
  }

  // Some firmwares answer IDENTIFY instead of pushing a banner.
  if (identifyMode(tag, 1800) && isAppMode(tag)) {
    moduleTag = tag;
    addLog("HM application identified: " + tag);
    return true;
  }

  return false;
}

bool requestHmFrame(uint8_t dst, uint8_t cmd, uint8_t expectedDst, uint8_t expectedCmd,
                    uint8_t *frame, size_t cap, size_t &len, uint32_t timeoutMs = 1800) {
  static uint8_t counter = 0xA0;
  while (HM.available()) HM.read();
  hmSendCommand(dst, counter++, &cmd, 1);
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, cap, len, 500)) continue;
    if (len >= 8 && frame[3] == expectedDst && frame[5] == expectedCmd) return true;
  }
  return false;
}

bool queryFirmwareVersion(String &versionOut) {
  uint8_t frame[2300];
  size_t len = 0;

  if (moduleTag == "DualCoPro_App" || moduleTag == "HMIP_TRX_App") {
    if (requestHmFrame(HM_DST_TRX, HM_CMD_TRX_GET_VERSION,
                       HM_DST_TRX, HM_CMD_TRX_ACK, frame, sizeof(frame), len)) {
      if (len >= 12 && frame[6] == 0x01) {
        char v[20];
        snprintf(v, sizeof(v), "%u.%u.%u", frame[7], frame[8], frame[9]);
        versionOut = v;
        moduleFirmware = versionOut;
        return true;
      }
    }
  }

  // Legacy Co_CPU_App fallback.
  if (requestHmFrame(HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_GET_VERSION,
                     HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_ACK, frame, sizeof(frame), len)) {
    if (len >= 15 && frame[6] == 0x02) {
      char v[20];
      snprintf(v, sizeof(v), "%u.%u.%u", frame[10], frame[11], frame[12]);
      versionOut = v;
      moduleFirmware = versionOut;
      return true;
    }
  }

  return false;
}

bool queryModuleInfo() {
  if (!isAppMode(moduleTag) && !enterApplication()) return false;

  uint8_t frame[2300];
  size_t len = 0;
  bool any = false;

  String version;
  if (queryFirmwareVersion(version)) any = true;

  if (moduleTag == "DualCoPro_App" || moduleTag == "HMIP_TRX_App") {
    if (requestHmFrame(HM_DST_TRX, HM_CMD_TRX_GET_MCU_TYPE,
                       HM_DST_TRX, HM_CMD_TRX_ACK, frame, sizeof(frame), len) &&
        len >= 10 && frame[6] == 0x01) {
      moduleMcuType = frame[7];
      if (moduleMcuType == 3) moduleTypeName = "HM-MOD-RPI-PCB";
      else if (moduleMcuType == 4) moduleTypeName = "RPI-RF-MOD";
      else if (moduleMcuType == 1) moduleTypeName = "HmIP-RFUSB";
      any = true;
    }

    if (requestHmFrame(HM_DST_HMIP, HM_CMD_HMIP_GET_DEFAULT_RF_ADDR,
                       HM_DST_HMIP, HM_CMD_HMIP_ACK, frame, sizeof(frame), len) &&
        len >= 11 && frame[6] == 0x01) {
      moduleHmIpAddress = ((uint32_t)frame[7] << 16) | ((uint32_t)frame[8] << 8) | frame[9];
      any = true;
    }

    if (requestHmFrame(HM_DST_COMMON, HM_CMD_COMMON_GET_SGTIN,
                       HM_DST_COMMON, HM_CMD_COMMON_ACK, frame, sizeof(frame), len) &&
        len >= 20 && frame[6] == 0x01) {
      char b[32];
      snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
               frame[7], frame[8], frame[9], frame[10], frame[11], frame[12],
               frame[13], frame[14], frame[15], frame[16], frame[17], frame[18]);
      moduleSGTIN = b;
      any = true;
    }

    if (requestHmFrame(HM_DST_LLMAC, HM_CMD_LLMAC_GET_DEFAULT_RF_ADDR,
                       HM_DST_LLMAC, HM_CMD_LLMAC_ACK, frame, sizeof(frame), len) &&
        len >= 11 && frame[6] == 0x01) {
      moduleBidCosAddress = ((uint32_t)frame[7] << 16) | ((uint32_t)frame[8] << 8) | frame[9];
      any = true;
    }

    if (requestHmFrame(HM_DST_LLMAC, HM_CMD_LLMAC_GET_SERIAL,
                       HM_DST_LLMAC, HM_CMD_LLMAC_ACK, frame, sizeof(frame), len) &&
        len >= 18 && frame[6] == 0x01) {
      char b[11];
      memcpy(b, frame + 7, 10);
      b[10] = 0;
      moduleSerial = b;
      any = true;
    }
  } else {
    if (requestHmFrame(HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_GET_SERIAL,
                       HM_DST_HMSYSTEM, HM_CMD_HMSYSTEM_ACK, frame, sizeof(frame), len) &&
        len >= 18 && frame[6] == 0x02) {
      char b[11];
      memcpy(b, frame + 7, 10);
      b[10] = 0;
      moduleSerial = b;
      any = true;
    }
  }

  if (any) {
    addLog("HM info: " + moduleTypeName + " FW " + moduleFirmware + " SN " + moduleSerial);
  }
  return any;
}

// ============================================================
// .eq3 parser / validator
//
// Format used by HM-MOD-UART firmware:
// 4 ASCII hex chars = block length
// followed by blockLength*2 ASCII hex chars.
// ============================================================
int hexNibble(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool readHexByte(File &f, uint8_t &out) {
  int a = f.read();
  int b = f.read();
  if (a < 0 || b < 0) return false;

  int hi = hexNibble(a);
  int lo = hexNibble(b);
  if (hi < 0 || lo < 0) return false;

  out = (uint8_t)((hi << 4) | lo);
  return true;
}

bool readBlockLength(File &f, uint16_t &blockLen) {
  int c[4];
  for (int i = 0; i < 4; i++) {
    c[i] = f.read();
    if (c[i] < 0) return false;
    if (hexNibble(c[i]) < 0) return false;
  }

  blockLen =
      ((uint16_t)hexNibble(c[0]) << 12) |
      ((uint16_t)hexNibble(c[1]) << 8)  |
      ((uint16_t)hexNibble(c[2]) << 4)  |
      ((uint16_t)hexNibble(c[3]));

  return true;
}

bool validateFirmwareFile(const char *path, int &blocks, size_t &decodedBytes, String &error) {
  blocks = 0;
  decodedBytes = 0;
  error = "";

  File f = SPIFFS.open(path, FILE_READ);
  if (!f) {
    error = "Firmware-Datei kann nicht geöffnet werden.";
    return false;
  }

  while (f.available()) {
    uint16_t blockLen = 0;
    if (!readBlockLength(f, blockLen)) {
      error = "Ungültige Blocklänge/Hex-Daten.";
      f.close();
      return false;
    }

    if (blockLen < 2 || blockLen > MAX_FW_BLOCK) {
      error = "Ungültige Blockgröße: " + String(blockLen);
      f.close();
      return false;
    }

    for (uint16_t i = 0; i < blockLen; i++) {
      uint8_t dummy;
      if (!readHexByte(f, dummy)) {
        error = "Firmware-Datei endet mitten in Block " + String(blocks + 1);
        f.close();
        return false;
      }
    }

    blocks++;
    decodedBytes += blockLen;

    if (blocks > 4096) {
      error = "Zu viele Firmware-Blöcke.";
      f.close();
      return false;
    }
  }

  f.close();

  if (blocks == 0) {
    error = "Keine Firmware-Blöcke gefunden.";
    return false;
  }

  return true;
}

bool waitUpdateAck(uint32_t timeoutMs) {
  uint8_t frame[2300];
  size_t len = 0;
  uint32_t start = millis();

  while (millis() - start < timeoutMs) {
    if (!readHmDecodedFrame(frame, sizeof(frame), len, 600)) continue;

    // flash-hmmoduart expects payload 04 01 on OS destination.
    if (len >= 9 &&
        frame[3] == HM_DST_OS &&
        frame[5] == HM_CMD_HMSYSTEM_ACK &&
        frame[6] == 0x01) {
      return true;
    }
  }

  return false;
}

void disconnectAllHbClients() {
  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    clients[i] = HbClient();
  }
}

bool flashFirmwareNow() {
  addLog("HM firmware flash requested");
  flashBusy = true;
  maintenanceMode = true;
  flashError = "";
  flashPercent = 0;
  flashStatus = "OpenCCU-Verbindung gesperrt";
  disconnectAllHbClients();

  while (hbUdp.parsePacket() > 0) {
    while (hbUdp.available()) hbUdp.read();
  }

  int blocks = 0;
  size_t decodedBytes = 0;
  String err;

  if (!validateFirmwareFile(FW_PATH, blocks, decodedBytes, err)) {
    flashError = err;
    flashStatus = "Abbruch";
    flashBusy = false;
    maintenanceMode = false;
    return false;
  }

  fwBlockCount = blocks;
  flashStatus = "HM-MOD Bootloader wird gestartet";

  if (!enterBootloader()) {
    flashError = "Co_CPU_BL konnte nicht erreicht werden.";
    flashStatus = "Abbruch";
    flashBusy = false;
    maintenanceMode = false;
    return false;
  }

  File f = SPIFFS.open(FW_PATH, FILE_READ);
  if (!f) {
    flashError = "Firmware-Datei kann nicht geöffnet werden.";
    flashStatus = "Abbruch";
    flashBusy = false;
    maintenanceMode = false;
    return false;
  }

  static uint8_t blockData[MAX_FW_BLOCK];
  static uint8_t payload[MAX_FW_BLOCK + 1];
  uint8_t counter = 0;

  for (int block = 0; block < blocks; block++) {
    uint16_t blockLen = 0;

    if (!readBlockLength(f, blockLen) ||
        blockLen < 2 ||
        blockLen > MAX_FW_BLOCK) {
      flashError = "Lesefehler bei Block " + String(block + 1);
      f.close();
      flashStatus = "Abbruch";
      flashBusy = false;
      maintenanceMode = false;
      return false;
    }

    for (uint16_t i = 0; i < blockLen; i++) {
      if (!readHexByte(f, blockData[i])) {
        flashError = "Hex-Lesefehler bei Block " + String(block + 1);
        f.close();
        flashStatus = "Abbruch";
        flashBusy = false;
        maintenanceMode = false;
        return false;
      }
    }

    // Mirror flash-hmmoduart:
    // original block length -> cmd length = blockLen - 1
    // payload[0] = 0x05 (OS_UPDATE_FIRMWARE)
    // then first blockLen-2 bytes from firmware block;
    // final two firmware bytes are not included.
    size_t payloadLen = blockLen - 1;
    payload[0] = HM_CMD_UPDATE_FW;
    if (blockLen > 2) memcpy(payload + 1, blockData, blockLen - 2);

    while (HM.available()) HM.read();

    hmSendCommand(HM_DST_OS, counter++, payload, payloadLen);

    flashStatus = "Block " + String(block + 1) + " / " + String(blocks);
    flashPercent = (int)(((uint32_t)(block + 1) * 100U) / (uint32_t)blocks);

    if (!waitUpdateAck(2500)) {
      flashError = "Kein ACK 04 01 bei Block " + String(block + 1);
      f.close();
      flashStatus = "Abbruch";
      flashBusy = false;
      maintenanceMode = false;
      return false;
    }

    yield();
  }

  f.close();

  flashStatus = "Firmware übertragen - starte Application";
  delay(1000);

  if (!enterApplication()) {
    flashError = "Firmware wurde übertragen, Application (Co_CPU_App/DualCoPro_App) startet aber nicht.";
    flashStatus = "Prüfung fehlgeschlagen";
    flashBusy = false;
    maintenanceMode = true;
    return false;
  }

  if (!queryModuleInfo()) {
    flashError = "Application läuft, Modulinfo konnte aber nicht vollständig gelesen werden.";
    flashStatus = "Prüfung unvollständig";
    flashBusy = false;
    maintenanceMode = true;
    return false;
  }

  flashPercent = 100;
  flashStatus = "Erfolgreich - Firmware " + moduleFirmware + " / " + moduleTag;
  addLog("HM firmware flash OK: " + moduleFirmware + " / " + moduleTag);
  flashBusy = false;

  // Keep maintenance active until ESP reboots so OpenCCU cannot interrupt
  // the verified post-flash state.
  maintenanceMode = true;
  rebootPending = true;
  rebootAt = millis() + 7000;

  return true;
}

// ============================================================
// HB-RF-ETH
// ============================================================
bool sameEndpoint(const HbClient &c, const IPAddress &ip, uint16_t port) {
  return c.connected && c.ip == ip && c.port == port;
}

HbClient* findClient(const IPAddress &ip, uint16_t port) {
  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    if (sameEndpoint(clients[i], ip, port)) return &clients[i];
  }
  return nullptr;
}

void disconnectClient(HbClient &c) {
  if (c.connected) { statDisconnects++; addLog("HB client disconnected: " + c.ip.toString()); }
  c = HbClient();
}

HbClient* findOrCreateClient(const IPAddress &ip, uint16_t port) {
  HbClient *existing = findClient(ip, port);
  if (existing) return existing;

  // hb_rf_eth.ko opens a NEW UDP source port on every connect/reconnect try.
  // Its CONNECT reply timeout is only ~50 ms. Do not tear down a same-IP
  // session merely because the source port changed; rebind the existing slot.
  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    if (clients[i].connected && clients[i].ip == ip) {
      clients[i].port = port;
      clients[i].lastRxMs = millis();
      clients[i].started = false;
      return &clients[i];
    }
  }

  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    if (!clients[i].connected) {
      clients[i] = HbClient();
      clients[i].ip = ip;
      clients[i].port = port;
      clients[i].endpointId = 1;
      clients[i].lastRxMs = millis();
      return &clients[i];
    }
  }
  return nullptr;
}

void hbSendTyped(HbClient &c, uint8_t type, const uint8_t *payload, size_t plen) {
  uint8_t pkt[1500];
  if (plen + 4 > sizeof(pkt)) return;

  pkt[0] = type;
  pkt[1] = c.txCounter++;
  if (plen) memcpy(pkt + 2, payload, plen);

  uint16_t crc = crc16Bidcos(pkt, plen + 2);
  pkt[plen + 2] = (uint8_t)(crc >> 8);
  pkt[plen + 3] = (uint8_t)(crc & 0xFF);

  hbUdp.beginPacket(c.ip, c.port);
  hbUdp.write(pkt, plen + 4);
  hbUdp.endPacket();
}

void hbBroadcastHmFrame(const uint8_t *frame, size_t len) {
  if (maintenanceMode) return;

  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    HbClient &c = clients[i];
    if (c.connected && c.started) {
      hbSendTyped(c, T_FRAME, frame, len);
      statHmToHost++;
    }
  }
}

bool decodeHmWire(const uint8_t *enc, size_t encLen, uint8_t *dec, size_t cap, size_t &decLen) {
  decLen = 0;
  bool esc = false;
  for (size_t i = 0; i < encLen; i++) {
    uint8_t b = enc[i];
    if (i == 0) {
      if (b != 0xFD || cap < 1) return false;
      dec[decLen++] = b;
      continue;
    }
    if (!esc && b == 0xFC) { esc = true; continue; }
    if (esc) { b |= 0x80; esc = false; }
    if (decLen >= cap) return false;
    dec[decLen++] = b;
  }
  if (decLen < 7) return false;
  uint16_t calc = crc16Bidcos(dec, decLen - 2);
  uint16_t got = ((uint16_t)dec[decLen - 2] << 8) | dec[decLen - 1];
  return calc == got;
}

void inspectHmFrame(const uint8_t *frame, size_t len) {
  lastHmFrame = hexShort(frame, len);
  lastHmFrameMs = millis();

  uint8_t dec[2300];
  size_t dl = 0;
  if (!decodeHmWire(frame, len, dec, sizeof(dec), dl)) return;

  String tag;
  if (detectModeTagInFrame(dec, dl, tag)) moduleTag = tag;

  if (dl < 9) return;
  uint8_t dst = dec[3];
  uint8_t cmd = dec[5];

  // DualCoPro firmware version response.
  if (dst == HM_DST_TRX && cmd == HM_CMD_TRX_ACK && dec[6] == 0x01 && dl >= 12) {
    // GET_VERSION has a 10-byte response data field. Version is the first 3 bytes after status.
    uint16_t packetLen = ((uint16_t)dec[1] << 8) | dec[2];
    size_t dataLen = packetLen >= 3 ? packetLen - 3 : 0;
    if (dataLen == 10) {
      char v[20];
      snprintf(v, sizeof(v), "%u.%u.%u", dec[7], dec[8], dec[9]);
      moduleFirmware = v;
    } else if (dataLen == 2) {
      moduleMcuType = dec[7];
      if (moduleMcuType == 3) moduleTypeName = "HM-MOD-RPI-PCB";
    }
  }

  if (dst == HM_DST_HMSYSTEM && cmd == HM_CMD_HMSYSTEM_ACK && dec[6] == 0x02 && dl >= 15) {
    uint16_t packetLen = ((uint16_t)dec[1] << 8) | dec[2];
    size_t dataLen = packetLen >= 3 ? packetLen - 3 : 0;
    if (dataLen == 7) {
      char v[20];
      snprintf(v, sizeof(v), "%u.%u.%u", dec[10], dec[11], dec[12]);
      moduleFirmware = v;
    } else if (dataLen == 11 && dl >= 18) {
      char b[11]; memcpy(b, dec + 7, 10); b[10] = 0; moduleSerial = b;
    }
  }

  if (dst == HM_DST_LLMAC && cmd == HM_CMD_LLMAC_ACK && dec[6] == 0x01) {
    uint16_t packetLen = ((uint16_t)dec[1] << 8) | dec[2];
    size_t dataLen = packetLen >= 3 ? packetLen - 3 : 0;
    if (dataLen == 11 && dl >= 18) {
      char b[11]; memcpy(b, dec + 7, 10); b[10] = 0; moduleSerial = b;
    } else if (dataLen == 4 && dl >= 11) {
      moduleBidCosAddress = ((uint32_t)dec[7] << 16) | ((uint32_t)dec[8] << 8) | dec[9];
    }
  }

  if (dst == HM_DST_HMIP && cmd == HM_CMD_HMIP_ACK && dec[6] == 0x01 && dl >= 11) {
    moduleHmIpAddress = ((uint32_t)dec[7] << 16) | ((uint32_t)dec[8] << 8) | dec[9];
  }

  if (dst == HM_DST_COMMON && cmd == HM_CMD_COMMON_ACK && dec[6] == 0x01) {
    uint16_t packetLen = ((uint16_t)dec[1] << 8) | dec[2];
    size_t dataLen = packetLen >= 3 ? packetLen - 3 : 0;
    if (dataLen == 13 && dl >= 20) {
      char b[32];
      snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
               dec[7], dec[8], dec[9], dec[10], dec[11], dec[12],
               dec[13], dec[14], dec[15], dec[16], dec[17], dec[18]);
      moduleSGTIN = b;
    }
  }
}

void pumpHmUart() {
  if (maintenanceMode) return;

  while (HM.available()) {
    uint8_t b = (uint8_t)HM.read();

    if (hmParser.push(b)) {
      statUartFrames++;
      inspectHmFrame(hmParser.buf, hmParser.pos);
      hbBroadcastHmFrame(hmParser.buf, hmParser.pos);
      hmParser.reset();
    }
  }
}

void handleHbPacket(const uint8_t *data, size_t len,
                    const IPAddress &ip, uint16_t port) {
  if (maintenanceMode) return;
  if (len < 4) return;

  uint16_t crcCalc = crc16Bidcos(data, len - 2);
  uint16_t crcRecv = ((uint16_t)data[len - 2] << 8) | data[len - 1];

  if (crcCalc != crcRecv) {
    statBadCrc++;
    return;
  }

  uint8_t type = data[0];
  uint8_t requestCounter = data[1];
  const uint8_t *payload = data + 2;
  size_t plen = len - 4;

  HbClient *c = (type == T_CONNECT)
                  ? findOrCreateClient(ip, port)
                  : findClient(ip, port);

  if (!c) return;

  c->lastRxMs = millis();

  switch (type) {
    case T_CONNECT: {
      bool wasConnected = c->connected;
      c->connected = true;
      c->started = false;

      // IMPORTANT: hb_rf_eth.ko waits only about 50 ms for this reply.
      // Send the reply first; logging happens afterwards.
      if (plen == 1 && payload[0] == 1) {
        if (!wasConnected) c->endpointId += 2;
        uint8_t reply[2] = {1, requestCounter};
        hbSendTyped(*c, T_CONNECT, reply, sizeof(reply));
      }
      else if (plen == 2 && payload[0] == 2) {
        uint8_t clientEp = payload[1];

        // New session (0) gets a new endpoint id.
        // A reconnect carries the last endpoint id. Adopt it after an ESP
        // reboot so the Linux driver can recover without reloading the module.
        if (clientEp == 0) {
          if (!wasConnected) c->endpointId += 2;
        } else {
          c->endpointId = clientEp;
        }

        uint8_t reply[3] = {2, requestCounter, c->endpointId};
        hbSendTyped(*c, T_CONNECT, reply, sizeof(reply));
      }

      if (!wasConnected) {
        statConnects++;
        addLog("HB CONNECT: " + ip.toString());
      }
      break;
    }

    case T_DISCONNECT:
      disconnectClient(*c);
      break;

    case T_KEEPALIVE:
      break;

    case T_LED:
      break;

    case T_RESET:
      hmResetPulse("OpenCCU");
      hmParser.reset();
      break;

    case T_STARTCONN:
      c->started = true;
      addLog("HB STARTCONN: " + ip.toString());
      break;

    case T_STOPCONN:
      c->started = false;
      break;

    case T_FRAME:
      if (plen > 0) {
        lastHostFrame = hexShort(payload, plen);
        lastHostFrameMs = millis();
        HM.write(payload, plen);
        HM.flush();
        statHostToHm++;
      }
      break;

    default:
      statUnknown++;
      break;
  }
}

void pollHbUdp() {
  while (true) {
    int n = hbUdp.parsePacket();
    if (n <= 0) break;

    uint8_t pkt[1500];

    if (n > (int)sizeof(pkt)) {
      while (hbUdp.available()) hbUdp.read();
      continue;
    }

    int rd = hbUdp.read(pkt, n);
    if (rd <= 0) continue;

    if (!maintenanceMode) {
      handleHbPacket(pkt, (size_t)rd, hbUdp.remoteIP(), hbUdp.remotePort());
    }
  }
}

void hbKeepalive() {
  if (maintenanceMode) return;

  static uint32_t last = 0;
  uint32_t now = millis();

  if (now - last < 1000) return;
  last = now;

  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    HbClient &c = clients[i];
    if (!c.connected) continue;

    if (now - c.lastRxMs > 5000) {
      statKeepaliveTimeout++;
      disconnectClient(c);
      continue;
    }

    hbSendTyped(c, T_KEEPALIVE, nullptr, 0);
  }
}

// ============================================================
// Web UI
// ============================================================
String navBar() {
  return F("<nav><a href='/'>Dashboard</a><a href='/diagnostics'>Funk & Relay</a>"
           "<a href='/log'>System-Log</a><a href='/firmware'>HM-Firmware</a>"
           "<a href='/esp'>ESP-Update</a><a href='/settings'>Einstellungen</a></nav>");
}

String htmlHeader(const String &title, bool autoRefresh = false) {
  String h;
  h += F("<!doctype html><html><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (autoRefresh) h += F("<meta http-equiv='refresh' content='5'>");
  h += "<title>" + title + "</title>";
  h += F("<style>"
         ":root{color-scheme:light dark;--bg:#f5f6f8;--card:#fff;--fg:#1c2025;--muted:#65707d;--accent:#ef6c2f;--line:#d8dde3}"
         "@media(prefers-color-scheme:dark){:root{--bg:#111418;--card:#1a1f25;--fg:#edf1f5;--muted:#9ba7b4;--line:#343b44}}"
         "*{box-sizing:border-box}body{font-family:system-ui,Segoe UI,Arial;margin:0;background:var(--bg);color:var(--fg)}"
         ".wrap{max-width:1120px;margin:auto;padding:20px}h1{margin:8px 0 18px}h2{margin-top:24px}"
         "nav{display:flex;gap:8px;flex-wrap:wrap;margin:0 0 18px}nav a{padding:9px 12px;border:1px solid var(--line);border-radius:8px;text-decoration:none;color:var(--fg);background:var(--card)}"
         ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:12px}.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}"
         ".k{font-size:.82rem;color:var(--muted);margin-bottom:4px}.v{font-size:1.15rem;font-weight:650;word-break:break-word}"
         "table{border-collapse:collapse;width:100%;background:var(--card)}td,th{border:1px solid var(--line);padding:8px;text-align:left}"
         "code,pre{word-break:break-all;white-space:pre-wrap}.ok{color:#149447;font-weight:700}.bad{color:#d43b45;font-weight:700}.warn{background:#fff3cd;color:#493d09;padding:12px;border:1px solid #e0c36a;border-radius:8px}"
         "button,input{padding:9px 11px;margin:4px;border-radius:7px;border:1px solid var(--line)}button{cursor:pointer}progress{width:100%;height:24px}.small{font-size:.86rem;color:var(--muted)}"
         "</style></head><body><div class='wrap'>");
  h += navBar();
  return h;
}

String htmlFooter() { return F("</div></body></html>"); }

void clientSummary(int &active, int &started, String &ip, uint16_t &port, uint8_t &endpoint) {
  active = started = 0; ip = "-"; port = 0; endpoint = 0;
  for (int i = 0; i < MAX_HB_CLIENTS; i++) {
    if (clients[i].connected) {
      active++; ip = clients[i].ip.toString(); port = clients[i].port; endpoint = clients[i].endpointId;
    }
    if (clients[i].connected && clients[i].started) started++;
  }
}

String htmlPage() {
  int active, started; String clientIp; uint16_t clientPort; uint8_t endpoint;
  clientSummary(active, started, clientIp, clientPort, endpoint);

  String h = htmlHeader("HM-C3 Gateway v3.1", true);
  h += F("<h1>HM-MOD ESP32-C3 Gateway v3.1</h1><div class='grid'>");
  h += "<div class='card'><div class='k'>Gateway</div><div class='v'>" + WiFi.localIP().toString() + "</div><div class='small'>" + hostName + ".local · RSSI " + String(WiFi.RSSI()) + " dBm</div></div>";
  h += "<div class='card'><div class='k'>OpenCCU / HB-RF-ETH</div><div class='v'>" + String(active) + " verbunden / " + String(started) + " gestartet</div><div class='small'>" + clientIp + " · UDP 3008</div></div>";
  h += "<div class='card'><div class='k'>Funkmodul</div><div class='v'>" + moduleTypeName + "</div><div class='small'>" + moduleTag + " · Firmware " + moduleFirmware + "</div></div>";
  h += "<div class='card'><div class='k'>Serial / SGTIN</div><div class='v'>" + moduleSerial + "</div><div class='small'>" + moduleSGTIN + "</div></div>";
  h += "<div class='card'><div class='k'>HmIP-Adresse</div><div class='v'>" + hexAddress24(moduleHmIpAddress) + "</div></div>";
  h += "<div class='card'><div class='k'>BidCos-Adresse</div><div class='v'>" + hexAddress24(moduleBidCosAddress) + "</div></div>";
  h += "<div class='card'><div class='k'>Traffic</div><div class='v'>" + String(statHostToHm) + " → HM / " + String(statHmToHost) + " → Host</div><div class='small'>CRC-Fehler " + String(statBadCrc) + " · Drops " + String(statUartFrameDrops) + "</div></div>";
  h += "<div class='card'><div class='k'>System</div><div class='v'>Uptime " + formatUptime() + "</div><div class='small'>Heap " + String(ESP.getFreeHeap()/1024) + " kB · Reset " + resetReasonText() + "</div></div>";
  h += F("</div>");

  if (maintenanceMode) h += F("<p class='warn'><b>Wartungsmodus aktiv.</b> HB-RF-ETH ist vorübergehend gesperrt.</p>");

  h += F("<h2>Aktionen</h2><form method='POST' action='/probe' style='display:inline'><button>Modulinfo neu lesen</button></form>"
         "<form method='POST' action='/reset' style='display:inline'><button>HM-Modul Reset</button></form>"
         "<form method='POST' action='/reboot' style='display:inline'><button>ESP neu starten</button></form>");
  h += htmlFooter();
  return h;
}

String diagnosticsPage() {
  int active, started; String clientIp; uint16_t clientPort; uint8_t endpoint;
  clientSummary(active, started, clientIp, clientPort, endpoint);
  String h = htmlHeader("Funk & Relay", true);
  h += F("<h1>Funk & Relay</h1><table>");
  h += "<tr><th>HB Client</th><td>" + clientIp + ":" + String(clientPort) + "</td></tr>";
  h += "<tr><th>Endpoint-ID</th><td>" + String(endpoint) + "</td></tr>";
  h += "<tr><th>Connects / Disconnects</th><td>" + String(statConnects) + " / " + String(statDisconnects) + "</td></tr>";
  h += "<tr><th>Keepalive Timeouts</th><td>" + String(statKeepaliveTimeout) + "</td></tr>";
  h += "<tr><th>Host → HM Frames</th><td>" + String(statHostToHm) + "</td></tr>";
  h += "<tr><th>HM → Host Frames</th><td>" + String(statHmToHost) + "</td></tr>";
  h += "<tr><th>UART komplette Frames</th><td>" + String(statUartFrames) + "</td></tr>";
  h += "<tr><th>UART Frame Drops</th><td>" + String(statUartFrameDrops) + "</td></tr>";
  h += "<tr><th>HB CRC Fehler</th><td>" + String(statBadCrc) + "</td></tr>";
  h += "<tr><th>Resets gesamt</th><td>" + String(statResets) + "</td></tr>";
  h += "<tr><th>Reset durch OpenCCU</th><td>" + String(statResetOpenCCU) + "</td></tr>";
  h += "<tr><th>Reset durch WebUI</th><td>" + String(statResetWeb) + "</td></tr>";
  h += "<tr><th>Reset Startup</th><td>" + String(statResetStartup) + "</td></tr>";
  h += "<tr><th>Reset Updater</th><td>" + String(statResetUpdater) + "</td></tr>";
  h += "<tr><th>WLAN Reconnects</th><td>" + String(statWifiReconnects) + "</td></tr></table>";
  h += F("<h2>Letzte Frames</h2><table>");
  h += "<tr><th>Host → HM</th><td><code>" + lastHostFrame + "</code></td></tr>";
  h += "<tr><th>HM → Host</th><td><code>" + lastHmFrame + "</code></td></tr></table>";
  h += F("<form method='POST' action='/statsreset'><button>Diagnosezähler zurücksetzen</button></form>");
  h += htmlFooter();
  return h;
}

String logPage() {
  String h = htmlHeader("System-Log", true);
  h += F("<h1>System-Log</h1><pre class='card'>");
  if (!sysLogCount) h += "Noch keine Einträge.";
  else {
    int start = (sysLogHead + SYSLOG_LINES - sysLogCount) % SYSLOG_LINES;
    for (int i = 0; i < sysLogCount; i++) {
      int idx = (start + i) % SYSLOG_LINES;
      h += htmlEscape(String(sysLog[idx])); h += "\n";
    }
  }
  h += F("</pre><form method='POST' action='/logclear'><button>Log löschen</button></form>");
  h += htmlFooter();
  return h;
}

String firmwarePage() {
  String h = htmlHeader("HM-MOD Firmware-Updater");
  h += F("<h1>HM-MOD Firmware-Updater</h1><div class='warn'><b>Vor dem Flashen OpenCCU/RaspberryMatic stoppen.</b><br>"
         "Nur passende HM-MOD-UART/HM-MOD-RPI-PCB .eq3-Dateien verwenden.</div>");
  h += "<p>Modus: <b>" + moduleTag + "</b> · Firmware: <b>" + moduleFirmware + "</b> · Serial: <b>" + moduleSerial + "</b></p>";
  h += F("<h2>1. .eq3-Datei hochladen</h2><form method='POST' action='/fwupload' enctype='multipart/form-data'>"
         "<input type='file' name='firmware' accept='.eq3' required><button type='submit'>Datei hochladen</button></form>");
  h += "<p>Datei vorhanden: <b>" + String(fwUploaded ? "JA" : "NEIN") + "</b><br>Upload-Größe: " + String(fwUploadedBytes) + " Byte<br>Erkannte Blöcke: " + String(fwBlockCount) + "</p>";
  if (fwUploaded) {
    h += F("<h2>2. Flash starten</h2><div class='warn'>Während des Schreibens Stromversorgung nicht unterbrechen.</div>"
           "<form method='POST' action='/flash' onsubmit=\"return confirm('HM-Firmware wirklich schreiben?');\"><button type='submit'>Firmware jetzt flashen</button></form>");
  }
  h += F("<h2>Status</h2>");
  h += "<progress value='" + String(flashPercent) + "' max='100'></progress><p><b>" + flashStatus + "</b></p>";
  if (flashError.length()) h += "<p class='bad'>Fehler: " + htmlEscape(flashError) + "</p>";
  h += htmlFooter();
  return h;
}

String espUpdatePage() {
  String h = htmlHeader("ESP Firmware Update");
  h += F("<h1>ESP32-C3 Firmware-Update</h1><div class='warn'>Hier wird nur die ESP-Dongle-Firmware (.bin) aktualisiert, nicht die HM-Funkmodul-Firmware.</div>"
         "<form method='POST' action='/espupdate' enctype='multipart/form-data' onsubmit=\"return confirm('ESP-Firmware aktualisieren?');\">"
         "<input type='file' name='firmware' accept='.bin' required><button type='submit'>ESP .bin installieren</button></form>");
  h += "<p>Freier Heap: " + String(ESP.getFreeHeap()/1024) + " kB</p>";
  h += htmlFooter();
  return h;
}

String settingsPage() {
  String h = htmlHeader("Einstellungen");
  h += F("<h1>Einstellungen</h1><h2>Netzwerk</h2><form method='POST' action='/saveconfig'>");
  h += "<label>Hostname<br><input name='hostname' value='" + htmlEscape(hostName) + "'></label><br>";
  h += "<label>WLAN SSID<br><input name='ssid' value='" + htmlEscape(wifiSsid) + "'></label><br>";
  h += F("<label>WLAN Passwort<br><input name='pass' type='password' placeholder='leer = unverändert'></label><br>"
         "<button>Speichern & neu starten</button></form>");
  h += F("<p class='small'>Für OpenCCU empfiehlt sich eine feste DHCP-Zuordnung für 192.168.188.7 im Router.</p>");
  h += htmlFooter();
  return h;
}

File uploadFile;
bool espUpdateOk = false;
String espUpdateError = "";

void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send(200, "text/html; charset=utf-8", htmlPage()); });
  web.on("/diagnostics", HTTP_GET, []() { web.send(200, "text/html; charset=utf-8", diagnosticsPage()); });
  web.on("/log", HTTP_GET, []() { web.send(200, "text/html; charset=utf-8", logPage()); });
  web.on("/firmware", HTTP_GET, []() { web.send(200, "text/html; charset=utf-8", firmwarePage()); });
  web.on("/esp", HTTP_GET, []() { web.send(200, "text/html; charset=utf-8", espUpdatePage()); });
  web.on("/settings", HTTP_GET, []() { web.send(200, "text/html; charset=utf-8", settingsPage()); });

  web.on("/fwupload", HTTP_POST,
    []() {
      if (uploadFile) uploadFile.close();
      int blocks = 0; size_t decoded = 0; String err;
      if (validateFirmwareFile(FW_PATH, blocks, decoded, err)) {
        fwUploaded = true; fwBlockCount = blocks;
        flashStatus = "Firmware-Datei geprüft: " + String(blocks) + " Blöcke";
        flashError = ""; addLog("HM firmware file validated: " + String(blocks) + " blocks");
      } else {
        fwUploaded = false; fwBlockCount = 0; flashStatus = "Firmware-Datei ungültig"; flashError = err;
        addLog("HM firmware upload invalid: " + err);
      }
      web.sendHeader("Location", "/firmware"); web.send(303);
    },
    []() {
      HTTPUpload &up = web.upload();
      if (up.status == UPLOAD_FILE_START) {
        fwUploaded = false; fwUploadedBytes = 0; fwBlockCount = 0; flashError = "";
        SPIFFS.remove(FW_PATH); uploadFile = SPIFFS.open(FW_PATH, FILE_WRITE);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) { uploadFile.write(up.buf, up.currentSize); fwUploadedBytes += up.currentSize; }
      } else if (up.status == UPLOAD_FILE_END) {
        if (uploadFile) uploadFile.close();
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile) uploadFile.close(); SPIFFS.remove(FW_PATH); flashError = "Upload abgebrochen.";
      }
    }
  );

  web.on("/flash", HTTP_POST, []() {
    if (!fwUploaded || flashBusy) { web.send(409, "text/plain", "Keine gültige Firmware-Datei oder Flash bereits aktiv."); return; }
    bool ok = flashFirmwareNow();
    String h = htmlHeader(ok ? "Firmware erfolgreich" : "Firmware Fehler");
    if (ok) {
      h += "<h1 class='ok'>HM-Firmware erfolgreich übertragen</h1><p>Modus: <b>" + moduleTag + "</b><br>Firmware: <b>" + moduleFirmware + "</b><br>Serial: <b>" + moduleSerial + "</b></p>";
      h += F("<p>Der ESP startet in wenigen Sekunden neu. Danach OpenCCU wieder starten.</p>");
    } else {
      h += "<h1 class='bad'>Firmware-Update abgebrochen</h1><p>Fehler: <b>" + htmlEscape(flashError) + "</b></p>";
    }
    h += htmlFooter(); web.send(200, "text/html; charset=utf-8", h);
  });

  web.on("/espupdate", HTTP_POST,
    []() {
      String h = htmlHeader(espUpdateOk ? "ESP Update OK" : "ESP Update Fehler");
      if (espUpdateOk) {
        h += F("<h1 class='ok'>ESP-Firmware erfolgreich geschrieben</h1><p>Neustart in wenigen Sekunden.</p>");
        rebootPending = true; rebootAt = millis() + 2500;
      } else {
        h += "<h1 class='bad'>ESP-Update fehlgeschlagen</h1><p>" + htmlEscape(espUpdateError) + "</p>";
        maintenanceMode = false;
      }
      h += htmlFooter(); web.send(200, "text/html; charset=utf-8", h);
    },
    []() {
      HTTPUpload &up = web.upload();
      if (up.status == UPLOAD_FILE_START) {
        maintenanceMode = true; disconnectAllHbClients(); espUpdateOk = false; espUpdateError = "";
        addLog("ESP OTA upload start");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) espUpdateError = "Update.begin fehlgeschlagen";
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (!espUpdateError.length() && Update.write(up.buf, up.currentSize) != up.currentSize) espUpdateError = "Flash-Schreibfehler";
      } else if (up.status == UPLOAD_FILE_END) {
        if (!espUpdateError.length() && Update.end(true)) { espUpdateOk = true; addLog("ESP OTA successful"); }
        else if (!espUpdateError.length()) espUpdateError = "Update.end fehlgeschlagen";
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        Update.abort(); espUpdateError = "Upload abgebrochen";
      }
    }
  );

  web.on("/api/status", HTTP_GET, []() {
    int active, started; String clientIp; uint16_t p; uint8_t ep; clientSummary(active, started, clientIp, p, ep);
    String j = "{";
    j += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    j += "\"hostname\":\"" + hostName + "\",";
    j += "\"module\":\"" + moduleTypeName + "\",";
    j += "\"mode\":\"" + moduleTag + "\",";
    j += "\"firmware\":\"" + moduleFirmware + "\",";
    j += "\"serial\":\"" + moduleSerial + "\",";
    j += "\"hmip_address\":\"" + hexAddress24(moduleHmIpAddress) + "\",";
    j += "\"bidcos_address\":\"" + hexAddress24(moduleBidCosAddress) + "\",";
    j += "\"connected\":" + String(active) + ",\"started\":" + String(started) + ",";
    j += "\"host_to_hm\":" + String(statHostToHm) + ",\"hm_to_host\":" + String(statHmToHost) + ",";
    j += "\"crc_errors\":" + String(statBadCrc) + ",\"heap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"rssi\":" + String(WiFi.RSSI()) + ",\"maintenance\":" + String(maintenanceMode ? "true" : "false");
    j += "}"; web.send(200, "application/json", j);
  });

  web.on("/api/log", HTTP_GET, []() {
    String out;
    int start = (sysLogHead + SYSLOG_LINES - sysLogCount) % SYSLOG_LINES;
    for (int i = 0; i < sysLogCount; i++) { out += sysLog[(start + i) % SYSLOG_LINES]; out += "\n"; }
    web.send(200, "text/plain; charset=utf-8", out);
  });

  web.on("/probe", HTTP_POST, []() {
    if (flashBusy) { web.send(409, "text/plain", "Firmware-Update aktiv."); return; }
    maintenanceMode = true; disconnectAllHbClients();
    bool ok = enterApplication() && queryModuleInfo();
    hmResetPulse("Web"); hmParser.reset(); maintenanceMode = false;
    addLog(String("Manual module probe: ") + (ok ? "OK" : "FAILED"));
    web.sendHeader("Location", "/"); web.send(303);
  });

  web.on("/reset", HTTP_POST, []() {
    if (flashBusy) { web.send(409, "text/plain", "Firmware-Update aktiv."); return; }
    hmResetPulse("Web"); hmParser.reset(); web.sendHeader("Location", "/"); web.send(303);
  });

  web.on("/reboot", HTTP_POST, []() {
    if (flashBusy) { web.send(409, "text/plain", "Firmware-Update aktiv."); return; }
    web.send(200, "text/plain", "Restart..."); delay(300); ESP.restart();
  });

  web.on("/statsreset", HTTP_POST, []() {
    statBadCrc = statUnknown = statHostToHm = statHmToHost = statKeepaliveTimeout = statUartFrames = statUartFrameDrops = 0;
    statConnects = statDisconnects = 0; addLog("Diagnostic counters reset");
    web.sendHeader("Location", "/diagnostics"); web.send(303);
  });

  web.on("/logclear", HTTP_POST, []() {
    sysLogHead = sysLogCount = 0; web.sendHeader("Location", "/log"); web.send(303);
  });

  web.on("/saveconfig", HTTP_POST, []() {
    String newHost = web.arg("hostname"); newHost.trim();
    String newSsid = web.arg("ssid"); newSsid.trim();
    String newPass = web.arg("pass");
    prefs.begin("hmc3", false);
    if (newHost.length()) prefs.putString("hostname", newHost);
    if (newSsid.length()) prefs.putString("ssid", newSsid);
    if (newPass.length()) prefs.putString("pass", newPass);
    prefs.end();
    web.send(200, "text/plain", "Gespeichert. Neustart..."); delay(400); ESP.restart();
  });

  web.begin(); addLog("WebUI started");
}

// ============================================================
// WiFi
// ============================================================
void startConfigAp() {
  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", (uint16_t)(mac & 0xFFFF));

  String ap = "HM-C3-" + String(suffix);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap.c_str());

  Serial.println("Setup-AP: " + ap);
  Serial.println("Setup-IP: " + WiFi.softAPIP().toString());
}

void connectWifi() {
  prefs.begin("hmc3", true);
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  String savedHost = prefs.getString("hostname", "");
  prefs.end();

  uint64_t mac = ESP.getEfuseMac();
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04x", (uint16_t)(mac & 0xFFFF));
  hostName = savedHost.length() ? savedHost : ("hm-c3-" + String(suffix));

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);   // critical: hb_rf_eth CONNECT timeout is only ~50 ms
  WiFi.setHostname(hostName.c_str());

  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    uint32_t start = millis();
    Serial.print("WLAN");
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) { Serial.print("."); delay(250); }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = millis(); addLog("WiFi connected: " + WiFi.localIP().toString());
  } else {
    startConfigAp(); addLog("WiFi failed - configuration AP active");
  }
}

void maintainWifi() {
  static uint32_t lastTry = 0;
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiOkMs = millis();
    return;
  }
  if (!wifiSsid.length()) return;
  if (millis() - lastTry < 10000) return;

  lastTry = millis();
  statWifiReconnects++;
  addLog("WiFi reconnect attempt");

  // Do not call WiFi.disconnect() here. A brief transient status must not
  // deliberately tear down an otherwise recoverable HB-RF-ETH session.
  WiFi.reconnect();
}

void setupMdns() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!MDNS.begin(hostName.c_str())) return;

  MDNS.addService("hbrfeth", "udp", HB_PORT);
  MDNS.addServiceTxt("hbrfeth", "udp", "wire", "hb-rf-eth");
  MDNS.addServiceTxt("hbrfeth", "udp", "model", "HM-C3-v3.1.1");
  MDNS.addServiceTxt("hbrfeth", "udp", "radio", "HM-MOD-RPI-PCB");
  MDNS.addService("http", "tcp", HTTP_PORT);
}

// ============================================================
// Startup
// ============================================================
void startupHmCheck() {
  while (HM.available()) HM.read();

  hmResetPulse("Startup");
  String tag;
  if (waitForBootTag(tag, 1800)) {
    moduleTag = tag;
    addLog("HM boot mode: " + tag);
  }

  if (enterApplication()) {
    queryModuleInfo();
  } else {
    addLog("HM application start failed during startup probe");
  }

  // Return to bootloader-ready state for OpenCCU hardware detection.
  hmResetPulse("Startup");
  hmParser.reset();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("====================================");
  Serial.println(" HM-MOD ESP32-C3 GATEWAY v3.1.1 PRO");
  Serial.println(" HB-RF-ETH + DualCoPro + Diagnostics + OTA");
  Serial.println("====================================");

  pinMode(HM_RESET_PIN, OUTPUT);
  digitalWrite(HM_RESET_PIN, HIGH);

  HM.begin(HM_BAUD, SERIAL_8N1, HM_RX_PIN, HM_TX_PIN);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS FEHLER");
  } else {
    if (SPIFFS.exists(FW_PATH)) {
      int blocks = 0;
      size_t decoded = 0;
      String err;
      fwUploaded = validateFirmwareFile(FW_PATH, blocks, decoded, err);
      if (fwUploaded) {
        fwBlockCount = blocks;
        fwUploadedBytes = SPIFFS.open(FW_PATH, FILE_READ).size();
      }
    }
  }

  startupHmCheck();
  connectWifi();

  hbUdp.begin(HB_PORT);
  setupWeb();
  setupMdns();

  addLog("WiFi power save: OFF (HB-RF-ETH low latency)");
  addLog("Gateway ready: HB-RF-ETH UDP/3008");
  Serial.println("Dashboard: http://" + hostName + ".local/");
}

void loop() {
  // HB-RF-ETH first: Linux waits only ~50 ms for CONNECT.
  if (!flashBusy) {
    pollHbUdp();
    if (!maintenanceMode) {
      pumpHmUart();
      hbKeepalive();
    }
    web.handleClient();
  }

  maintainWifi();

  if (rebootPending && (int32_t)(millis() - rebootAt) >= 0) {
    delay(100);
    ESP.restart();
  }

  delay(1);
}