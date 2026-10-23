#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>
#include <Wire.h>
#include <U8g2lib.h>

// Copy secrets.example.h to secrets.h and fill in your own Wi-Fi and Adafruit IO key.
#include "secrets.h"

const char* STATUS_API_URL =
  "https://io.adafruit.com/api/v2/p4hamster/feeds/silent-booth-01-status/data";
const char* ADAFRUIT_IO_USERNAME = "p4hamster";

constexpr uint32_t RADAR_BAUD = 256000;
constexpr int RADAR_RX_PIN = 20;
constexpr int RADAR_TX_PIN = 21;
constexpr int OLED_SDA_PIN = 5;
constexpr int OLED_SCL_PIN = 6;
constexpr uint32_t OLED_REFRESH_MS = 80;
constexpr uint32_t OLED_WHISPER_DELAY_MS = 8000;
constexpr uint32_t OLED_WHISPER_PERIOD_MS = 24000;
constexpr uint32_t OLED_WHISPER_DURATION_MS = 1800;
constexpr uint32_t UPLOAD_INTERVAL_MS = 8000;
constexpr uint32_t STATE_CHANGE_UPLOAD_GAP_MS = 1000;
// Wi-Fi 库会自动重连；仅在长时间未连上时补发一次连接请求。
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 60000;
constexpr uint32_t RADAR_STALE_AFTER_MS = 3000;
constexpr uint32_t VALID_UNIX_TIME = 1700000000UL;

// 1×1 米静音舱专用判定：墙面反射不能直接算作人体。
constexpr uint16_t MIN_HUMAN_DISTANCE_CM = 20;
constexpr uint16_t MAX_HUMAN_DISTANCE_CM = 130;
constexpr uint16_t MAX_STATIC_DRIFT_CM = 80;
constexpr uint32_t ENTRY_CONFIRM_MS = 900;
constexpr uint32_t EXIT_CONFIRM_MS = 5000;

// LD2410C 每个距离门约 0.75 米；门 2 覆盖约 1.5 米并留少量安装余量。
constexpr uint8_t RADAR_MAX_GATE = 2;

HardwareSerial RadarSerial(1);
U8G2_SH1106_128X32_VISIONOX_F_HW_I2C oled(
  U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN
);

const uint8_t FRAME_HEADER[] = {0xF4, 0xF3, 0xF2, 0xF1};
const uint8_t FRAME_TAIL[] = {0xF8, 0xF7, 0xF6, 0xF5};
constexpr size_t MAX_FRAME_SIZE = 64;

uint8_t frameBuffer[MAX_FRAME_SIZE];
size_t frameLength = 0;
size_t expectedFrameLength = 0;

struct RadarReading {
  bool valid = false;
  bool occupied = false;
  uint8_t rawTargetState = 0;
  uint8_t targetState = 0;
  uint16_t movingDistanceCm = 0;
  uint16_t staticDistanceCm = 0;
  uint16_t detectionDistanceCm = 0;
  uint16_t acceptedDistanceCm = 0;
  uint32_t lastFrameMs = 0;
};

RadarReading radar;
uint32_t lastUploadAttemptMs = 0;
uint32_t lastUploadSuccessMs = 0;
uint32_t lastWiFiRetryMs = 0;
bool timeSyncStarted = false;
wl_status_t previousWiFiStatus = WL_NO_SHIELD;
bool occupancyStateInitialized = false;
bool previousOccupied = false;
time_t occupiedSinceEpoch = 0;
uint32_t entryCandidateSinceMs = 0;
uint32_t lastHumanEvidenceMs = 0;
uint16_t trackedHumanDistanceCm = 0;
bool statusDirty = true;
bool oledReady = false;
uint32_t lastOledRefreshMs = 0;
uint32_t faceTransitionMs = 0;
uint32_t faceStateSinceMs = 0;

enum FaceState : uint8_t { FACE_SCANNING, FACE_EMPTY, FACE_OCCUPIED, FACE_RADAR_ERROR };
FaceState faceTarget = FACE_SCANNING;

// 屏幕只读取已过滤的雷达状态，不参与占用判定和公网上报。
FaceState currentFaceState(uint32_t nowMs) {
  if (!radar.valid) return nowMs < RADAR_STALE_AFTER_MS
    ? FACE_SCANNING : FACE_RADAR_ERROR;
  if (nowMs - radar.lastFrameMs > RADAR_STALE_AFTER_MS) return FACE_RADAR_ERROR;
  return radar.occupied ? FACE_OCCUPIED : FACE_EMPTY;
}

void initializeOled() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(100000);
  // 常见的 I2C 地址；找不到屏幕时雷达和网络仍照常运行。
  uint8_t address = 0;
  for (uint8_t candidate : {0x3C, 0x3D}) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) {
      address = candidate;
      break;
    }
  }
  if (address == 0) {
    Serial.println("[OLED] GPIO5/6 未发现 I2C 屏幕，跳过显示");
    return;
  }
  oled.setI2CAddress(address << 1);
  oled.begin();
  oled.setBusClock(400000);
  oledReady = true;
  Serial.printf("[OLED] SH1106 128x32 已启动，I2C 地址 0x%02X\n", address);
}

void drawHeart(int x, int y, bool pulse) {
  const char* small[] = {
    ".##.##.", "#######", "#######", ".#####.", "..###..", "...#..."
  };
  const char* large[] = {
    ".###.###.", "#########", "#########", ".#######.",
    "..#####..", "...###...", "....#...."
  };
  const char** rows = pulse ? large : small;
  const uint8_t height = pulse ? 7 : 6;
  const uint8_t width = pulse ? 9 : 7;
  for (uint8_t row = 0; row < height; ++row) {
    for (uint8_t col = 0; col < width; ++col) {
      if (rows[row][col] == '#') oled.drawPixel(x + col, y + row);
    }
  }
}

void drawQuestion(int x, int y) {
  const char* rows[] = {
    ".###.", "#...#", "...#.", "..#..", "..#..", ".....", "..#.."
  };
  for (uint8_t row = 0; row < 7; ++row) {
    for (uint8_t col = 0; col < 5; ++col) {
      if (rows[row][col] == '#') oled.drawPixel(x + col, y + row);
    }
  }
}

void faceLine(int x0, int y0, int x1, int y1, int dy, bool thick = true) {
  oled.drawLine(x0, y0 + dy, x1, y1 + dy);
  if (thick) oled.drawLine(x0, y0 + dy + 1, x1, y1 + dy + 1);
}

void drawWhisper(FaceState state, uint32_t nowMs) {
  const uint32_t stateAge = nowMs - faceStateSinceMs;
  if (stateAge < OLED_WHISPER_DELAY_MS) return;
  const uint32_t cycleAge = stateAge - OLED_WHISPER_DELAY_MS;
  const uint32_t visibleAge = cycleAge % OLED_WHISPER_PERIOD_MS;
  if (visibleAge >= OLED_WHISPER_DURATION_MS) return;

  const bool alternate = (cycleAge / OLED_WHISPER_PERIOD_MS) % 2;
  const char* phrase = "hello?";
  if (state == FACE_EMPTY) phrase = alternate ? "hi there" : "come in?";
  if (state == FACE_OCCUPIED) phrase = alternate ? "cozy here" : "shh...";
  if (state == FACE_SCANNING) phrase = alternate ? "looking..." : "hello?";
  if (state == FACE_RADAR_ERROR) phrase = alternate ? "hmm..." : "you there?";

  // 小字从右下角逐字出现，不遮挡中间的表情。
  char visible[16] = {};
  const size_t length = strlen(phrase);
  const size_t count = min(length, static_cast<size_t>(visibleAge / 95 + 1));
  memcpy(visible, phrase, count);
  oled.setFont(u8g2_font_5x7_tr);
  oled.drawStr(126 - oled.getStrWidth(visible), 31, visible);
}

void drawFace(FaceState state, uint32_t nowMs) {
  const uint32_t transitionAge = nowMs - faceTransitionMs;
  const bool transitioning = transitionAge < 200;
  const bool blink = transitioning && transitionAge < 90;
  const bool wink = transitioning && !blink;
  const bool naturalBlink = !transitioning && nowMs % 5200 < 90;
  // 每隔一段时间仅上下移动 1 像素，避免影响辨识。
  const int dy = (!transitioning && (nowMs / 800) % 4 == 2) ? -1 : 0;

  oled.clearBuffer();
  if (state == FACE_OCCUPIED) {
    if (blink || naturalBlink) {
      faceLine(39, 13, 50, 13, dy);
      faceLine(77, 13, 88, 13, dy);
    } else {
      faceLine(39, 16, 45, 10, dy);
      faceLine(45, 10, 51, 16, dy);
      if (wink) {
        faceLine(77, 13, 89, 13, dy);
      } else {
        faceLine(77, 16, 83, 10, dy);
        faceLine(83, 10, 89, 16, dy);
      }
    }
    faceLine(53, 22, 57, 25, dy);
    faceLine(57, 25, 61, 26, dy);
    faceLine(61, 26, 66, 26, dy);
    faceLine(66, 26, 70, 24, dy);
    faceLine(70, 24, 74, 21, dy);
    const bool pulse = (nowMs / 650) % 2 == 1;
    drawHeart(pulse ? 109 : 111, pulse ? 2 : 3, pulse);
  } else if (state == FACE_RADAR_ERROR) {
    faceLine(40, 11, 48, 11, dy);
    faceLine(42, 16, 50, 16, dy);
    faceLine(78, 11, 86, 11, dy);
    faceLine(76, 16, 84, 16, dy);
    faceLine(62, 24, 65, 24, dy, false);
    drawQuestion(109, 3);
    faceLine(117, 20, 119, 17, 0, false);
    faceLine(119, 17, 121, 20, 0, false);
    faceLine(121, 20, 121, 22, 0, false);
    faceLine(121, 22, 117, 22, 0, false);
    faceLine(117, 22, 117, 20, 0, false);
  } else {
    if (blink || naturalBlink) {
      faceLine(39, 13, 50, 13, dy);
      faceLine(77, 13, 88, 13, dy);
    } else {
      oled.drawRBox(39, 10 + dy, 12, 5, 2);
      oled.drawRBox(77, 10 + dy, 12, 5, 2);
    }
    if (state == FACE_SCANNING) {
      oled.drawCircle(63, 24 + dy, 2);
      drawQuestion(110, 3);
    } else {
      faceLine(60, 24, 67, 24, dy);
    }
  }
  drawWhisper(state, nowMs);
  // 角落诊断：W=未连接热点，C=已连热点但云端长时间无成功上报。
  // 联网正常时不显示字符，保持表情为屏幕主体。
  if (WiFi.status() != WL_CONNECTED) {
    oled.setFont(u8g2_font_5x7_tr);
    oled.drawStr(2, 7, "W");
  } else if (lastUploadSuccessMs == 0 ||
             nowMs - lastUploadSuccessMs > 60000) {
    oled.setFont(u8g2_font_5x7_tr);
    oled.drawStr(2, 7, "C");
  }
  oled.sendBuffer();
}

void updateOled(uint32_t nowMs) {
  if (!oledReady || nowMs - lastOledRefreshMs < OLED_REFRESH_MS) return;
  lastOledRefreshMs = nowMs;
  const FaceState next = currentFaceState(nowMs);
  if (next != faceTarget) {
    faceTarget = next;
    faceTransitionMs = nowMs;
    faceStateSinceMs = nowMs;
  }
  drawFace(faceTarget, nowMs);
}

const char* targetTypeName(uint8_t state) {
  switch (state) {
    case 0x01: return "moving";
    case 0x02: return "static";
    case 0x03: return "both";
    default: return "none";
  }
}

uint16_t currentDistanceCm() {
  return radar.occupied ? radar.acceptedDistanceCm : 0;
}

bool distanceInsideBooth(uint16_t distanceCm) {
  return distanceCm >= MIN_HUMAN_DISTANCE_CM &&
         distanceCm <= MAX_HUMAN_DISTANCE_CM;
}

bool distanceNearTrackedHuman(uint16_t distanceCm) {
  if (trackedHumanDistanceCm == 0) return true;
  const uint16_t difference = distanceCm > trackedHumanDistanceCm
    ? distanceCm - trackedHumanDistanceCm
    : trackedHumanDistanceCm - distanceCm;
  return difference <= MAX_STATIC_DRIFT_CM;
}

void setFilteredOccupancy(bool occupied, uint32_t nowMs) {
  if (!occupancyStateInitialized || occupied != previousOccupied) {
    occupancyStateInitialized = true;
    previousOccupied = occupied;
    statusDirty = true;
    const time_t now = time(nullptr);
    occupiedSinceEpoch = occupied && now >= VALID_UNIX_TIME ? now : 0;
    if (Serial) {
      Serial.println(occupied
        ? "[判定] 运动证据持续满足，确认有人"
        : "[判定] 连续无人体证据，确认空舱");
    }
  }

  radar.occupied = occupied;
  if (!occupied) {
    radar.targetState = 0;
    radar.acceptedDistanceCm = 0;
    trackedHumanDistanceCm = 0;
    lastHumanEvidenceMs = 0;
    entryCandidateSinceMs = 0;
  } else if (lastHumanEvidenceMs == 0) {
    lastHumanEvidenceMs = nowMs;
  }
}

void updateHumanOccupancy(uint8_t rawState, uint32_t nowMs) {
  const bool movingInBooth =
    (rawState == 0x01 || rawState == 0x03) &&
    distanceInsideBooth(radar.movingDistanceCm);
  const bool staticInBooth =
    (rawState == 0x02 || rawState == 0x03) &&
    distanceInsideBooth(radar.staticDistanceCm);

  if (!radar.occupied) {
    // 静止信号可能来自墙面，不能单独开启一次“有人”状态。
    if (!movingInBooth) {
      entryCandidateSinceMs = 0;
      radar.targetState = 0;
      radar.acceptedDistanceCm = 0;
      return;
    }

    if (entryCandidateSinceMs == 0) entryCandidateSinceMs = nowMs;
    if (nowMs - entryCandidateSinceMs < ENTRY_CONFIRM_MS) return;

    trackedHumanDistanceCm = radar.movingDistanceCm;
    radar.targetState = staticInBooth ? 0x03 : 0x01;
    radar.acceptedDistanceCm = radar.movingDistanceCm;
    lastHumanEvidenceMs = nowMs;
    setFilteredOccupancy(true, nowMs);
    return;
  }

  bool hasHumanEvidence = false;
  uint8_t acceptedState = 0;
  uint16_t acceptedDistance = 0;

  if (movingInBooth) {
    hasHumanEvidence = true;
    acceptedState = 0x01;
    acceptedDistance = radar.movingDistanceCm;
    trackedHumanDistanceCm = radar.movingDistanceCm;
  }

  if (staticInBooth && distanceNearTrackedHuman(radar.staticDistanceCm)) {
    hasHumanEvidence = true;
    acceptedState = acceptedState == 0x01 ? 0x03 : 0x02;
    if (acceptedDistance == 0) acceptedDistance = radar.staticDistanceCm;
  }

  if (hasHumanEvidence) {
    radar.targetState = acceptedState;
    radar.acceptedDistanceCm = acceptedDistance;
    lastHumanEvidenceMs = nowMs;
    return;
  }

  if (lastHumanEvidenceMs > 0 &&
      nowMs - lastHumanEvidenceMs >= EXIT_CONFIRM_MS) {
    setFilteredOccupancy(false, nowMs);
  }
}

void resetFrameParser() {
  frameLength = 0;
  expectedFrameLength = 0;
}

bool hasValidTail() {
  if (expectedFrameLength < 4) return false;
  const size_t tailStart = expectedFrameLength - 4;
  for (size_t i = 0; i < 4; i++) {
    if (frameBuffer[tailStart + i] != FRAME_TAIL[i]) return false;
  }
  return true;
}

void parseCompleteFrame() {
  // 常规数据帧至少含 13 字节数据区，0x02 表示常规数据，0xAA 是区内帧头。
  if (expectedFrameLength < 23 ||
      frameBuffer[6] != 0x02 ||
      frameBuffer[7] != 0xAA ||
      frameBuffer[17] != 0x55 ||
      !hasValidTail()) {
    return;
  }

  const uint8_t state = frameBuffer[8];
  if (state > 0x03) return;

  radar.rawTargetState = state;
  radar.movingDistanceCm =
    static_cast<uint16_t>(frameBuffer[9]) |
    (static_cast<uint16_t>(frameBuffer[10]) << 8);
  radar.staticDistanceCm =
    static_cast<uint16_t>(frameBuffer[12]) |
    (static_cast<uint16_t>(frameBuffer[13]) << 8);
  radar.detectionDistanceCm =
    static_cast<uint16_t>(frameBuffer[15]) |
    (static_cast<uint16_t>(frameBuffer[16]) << 8);
  radar.lastFrameMs = millis();
  radar.valid = true;
  updateHumanOccupancy(state, radar.lastFrameMs);
}

void processRadarByte(uint8_t value) {
  if (frameLength < 4) {
    if (value == FRAME_HEADER[frameLength]) {
      frameBuffer[frameLength++] = value;
    } else {
      frameLength = (value == FRAME_HEADER[0]) ? 1 : 0;
      if (frameLength == 1) frameBuffer[0] = value;
    }
    return;
  }

  if (frameLength >= MAX_FRAME_SIZE) {
    resetFrameParser();
    return;
  }

  frameBuffer[frameLength++] = value;

  if (frameLength == 6) {
    const uint16_t dataLength =
      static_cast<uint16_t>(frameBuffer[4]) |
      (static_cast<uint16_t>(frameBuffer[5]) << 8);
    expectedFrameLength = 4 + 2 + dataLength + 4;

    if (dataLength < 13 || expectedFrameLength > MAX_FRAME_SIZE) {
      resetFrameParser();
    }
    return;
  }

  if (expectedFrameLength > 0 && frameLength == expectedFrameLength) {
    parseCompleteFrame();
    resetFrameParser();
  }
}

bool waitForRadarAck(uint8_t expectedCommand, uint32_t timeoutMs) {
  const uint8_t ackHeader[] = {0xFD, 0xFC, 0xFB, 0xFA};
  uint8_t ack[64];
  size_t ackLength = 0;
  size_t ackExpectedLength = 0;
  const uint32_t startedMs = millis();

  while (millis() - startedMs < timeoutMs) {
    if (RadarSerial.available() == 0) {
      delay(1);
      continue;
    }

    const uint8_t value = static_cast<uint8_t>(RadarSerial.read());
    if (ackLength < 4) {
      if (value == ackHeader[ackLength]) {
        ack[ackLength++] = value;
      } else {
        ackLength = value == ackHeader[0] ? 1 : 0;
        if (ackLength == 1) ack[0] = value;
      }
      continue;
    }

    if (ackLength >= sizeof(ack)) {
      ackLength = 0;
      ackExpectedLength = 0;
      continue;
    }
    ack[ackLength++] = value;

    if (ackLength == 6) {
      const uint16_t dataLength =
        static_cast<uint16_t>(ack[4]) |
        (static_cast<uint16_t>(ack[5]) << 8);
      ackExpectedLength = 4 + 2 + dataLength + 4;
      if (ackExpectedLength > sizeof(ack)) {
        ackLength = 0;
        ackExpectedLength = 0;
      }
      continue;
    }

    if (ackExpectedLength > 0 && ackLength == ackExpectedLength) {
      return ackLength >= 12 &&
             ack[6] == expectedCommand &&
             ack[7] == 0x01 &&
             ack[8] == 0x00 &&
             ack[9] == 0x00;
    }
  }
  return false;
}

bool sendRadarConfigCommand(
  uint8_t command,
  const uint8_t* value,
  size_t valueLength
) {
  uint8_t commandFrame[40];
  const size_t dataLength = 2 + valueLength;
  const size_t totalLength = 4 + 2 + dataLength + 4;
  if (totalLength > sizeof(commandFrame)) return false;

  while (RadarSerial.available() > 0) RadarSerial.read();

  commandFrame[0] = 0xFD;
  commandFrame[1] = 0xFC;
  commandFrame[2] = 0xFB;
  commandFrame[3] = 0xFA;
  commandFrame[4] = static_cast<uint8_t>(dataLength & 0xFF);
  commandFrame[5] = static_cast<uint8_t>((dataLength >> 8) & 0xFF);
  commandFrame[6] = command;
  commandFrame[7] = 0x00;
  for (size_t i = 0; i < valueLength; i++) {
    commandFrame[8 + i] = value[i];
  }
  const size_t tailStart = 8 + valueLength;
  commandFrame[tailStart] = 0x04;
  commandFrame[tailStart + 1] = 0x03;
  commandFrame[tailStart + 2] = 0x02;
  commandFrame[tailStart + 3] = 0x01;

  RadarSerial.write(commandFrame, totalLength);
  RadarSerial.flush();
  return waitForRadarAck(command, 800);
}

bool setRadarGateSensitivity(
  uint16_t gate,
  uint8_t movingSensitivity,
  uint8_t staticSensitivity
) {
  const uint8_t value[] = {
    0x00, 0x00,
    static_cast<uint8_t>(gate & 0xFF),
    static_cast<uint8_t>((gate >> 8) & 0xFF),
    0x00, 0x00,
    0x01, 0x00,
    movingSensitivity, 0x00, 0x00, 0x00,
    0x02, 0x00,
    staticSensitivity, 0x00, 0x00, 0x00,
  };
  return sendRadarConfigCommand(0x64, value, sizeof(value));
}

void configureRadarForSilentBooth() {
  const uint8_t enableConfiguration[] = {0x01, 0x00};
  bool success = sendRadarConfigCommand(
    0xFF,
    enableConfiguration,
    sizeof(enableConfiguration)
  );

  const uint8_t rangeConfiguration[] = {
    0x00, 0x00, RADAR_MAX_GATE, 0x00, 0x00, 0x00,
    0x01, 0x00, RADAR_MAX_GATE, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x02, 0x00, 0x00, 0x00,
  };
  success = sendRadarConfigCommand(
    0x60,
    rangeConfiguration,
    sizeof(rangeConfiguration)
  ) && success;

  // 先关闭所有距离门，再只打开舱内 0～2 门；数值越高越不敏感。
  success = setRadarGateSensitivity(0xFFFF, 100, 100) && success;
  success = setRadarGateSensitivity(0, 45, 70) && success;
  success = setRadarGateSensitivity(1, 45, 65) && success;
  success = setRadarGateSensitivity(2, 50, 70) && success;
  success = sendRadarConfigCommand(0xFE, nullptr, 0) && success;

  resetFrameParser();
  while (RadarSerial.available() > 0) RadarSerial.read();
  Serial.println(success
    ? "[雷达配置] 已限制为舱内 0～1.5 米并提高静止阈值"
    : "[雷达配置] 有指令未确认，继续运行并等待人工复查");
}

void readRadar() {
  while (RadarSerial.available() > 0) {
    processRadarByte(static_cast<uint8_t>(RadarSerial.read()));
  }
}

bool systemTimeReady() {
  return static_cast<uint32_t>(time(nullptr)) >= VALID_UNIX_TIME;
}

void maintainWiFi() {
  const wl_status_t currentStatus = WiFi.status();

  if (currentStatus != previousWiFiStatus) {
    if (currentStatus == WL_CONNECTED) {
      Serial.print("[Wi-Fi] 已连接，IP: ");
      Serial.println(WiFi.localIP());
    } else if (previousWiFiStatus == WL_CONNECTED) {
      Serial.println("[Wi-Fi] 连接已断开，准备自动重连");
    }
    previousWiFiStatus = currentStatus;
  }

  if (currentStatus == WL_CONNECTED) {
    if (!timeSyncStarted) {
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      timeSyncStarted = true;
      Serial.println("[时间] 已启动网络校时");
    }
    return;
  }

  const uint32_t nowMs = millis();
  if (nowMs - lastWiFiRetryMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiRetryMs = nowMs;
    Serial.print("[Wi-Fi] 连接超时，状态码=");
    Serial.print(static_cast<int>(currentStatus));
    Serial.println("，重新发起连接（不主动断开）...");
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void reportWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.print("[Wi-Fi] 断开原因码=");
    Serial.println(info.wifi_sta_disconnected.reason);
  }
}

void uploadStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  const uint32_t nowMs = millis();
  if (!radar.valid || nowMs - radar.lastFrameMs > RADAR_STALE_AFTER_MS) {
    if (Serial) {
      Serial.println("[雷达] 暂无新鲜数据，本次不上报；持续无数据后网页会显示离线");
    }
    return;
  }

  if (radar.occupied && systemTimeReady() &&
      occupiedSinceEpoch < VALID_UNIX_TIME) {
    occupiedSinceEpoch = time(nullptr);
  } else if (!radar.occupied) {
    occupiedSinceEpoch = 0;
  }

  const uint16_t reportedDistanceCm = currentDistanceCm();
  char distanceValue[12];
  if (radar.occupied && reportedDistanceCm > 0) {
    snprintf(distanceValue, sizeof(distanceValue), "%u", reportedDistanceCm);
  } else {
    snprintf(distanceValue, sizeof(distanceValue), "null");
  }

  // Adafruit IO 的 value 字段保存一整条状态 JSON，单次上报只计一个数据点。
  char payload[512];
  snprintf(
    payload,
    sizeof(payload),
    "{\"value\":\"{\\\"device_id\\\":\\\"booth-01\\\","
    "\\\"occupied\\\":%s,\\\"target_type\\\":\\\"%s\\\","
    "\\\"distance_cm\\\":%s,\\\"uptime_ms\\\":%lu,"
    "\\\"occupied_since_unix\\\":%lld}\"}",
    radar.occupied ? "true" : "false",
    targetTypeName(radar.targetState),
    distanceValue,
    static_cast<unsigned long>(nowMs),
    static_cast<long long>(occupiedSinceEpoch)
  );

  NetworkClientSecure secureClient;
  // 热点网络与证书链已实机验证；此项目不传输个人信息，优先保证兼容性。
  secureClient.setInsecure();
  secureClient.setHandshakeTimeout(10);

  HTTPClient https;
  https.setConnectTimeout(10000);
  https.setTimeout(10000);

  if (!https.begin(secureClient, STATUS_API_URL)) {
    Serial.println("[HTTPS] 无法创建连接");
    return;
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("X-AIO-Key", ADAFRUIT_IO_KEY);
  https.addHeader("User-Agent", "SilentBooth-ESP32/1.0");

  const int statusCode =
    https.POST(reinterpret_cast<uint8_t*>(payload), strlen(payload));

  if (statusCode >= 200 && statusCode < 300) {
    statusDirty = false;
    lastUploadSuccessMs = millis();
    if (Serial) {
      Serial.print("[上报] 成功，HTTP ");
      Serial.print(statusCode);
      Serial.print("，状态=");
      Serial.print(radar.occupied ? "使用中" : "空闲");
      Serial.print("，类型=");
      Serial.print(targetTypeName(radar.targetState));
      Serial.print("，距离=");
      if (reportedDistanceCm > 0) {
        Serial.print(reportedDistanceCm);
        Serial.print(" cm");
      } else {
        Serial.print("—");
      }
      Serial.print("，原始=");
      Serial.print(targetTypeName(radar.rawTargetState));
      Serial.print("，运动距离=");
      Serial.print(radar.movingDistanceCm);
      Serial.print("，静止距离=");
      Serial.println(radar.staticDistanceCm);
    }
  } else {
    if (Serial) {
      Serial.print("[上报] 失败，HTTP ");
      Serial.println(statusCode);
      Serial.print("[上报] 错误说明：");
      Serial.println(https.errorToString(statusCode));
      const String response = https.getString();
      if (response.length() > 0) {
        Serial.print("[上报] 服务端返回: ");
        Serial.println(response.substring(0, 240));
      }
    }
  }

  https.end();
}

void setup() {
  Serial.begin(115200);
  // USB 接着电脑但未打开串口监视器时，日志不得阻塞雷达读取和网络上报。
  Serial.setTxTimeoutMs(0);
  delay(500);
  Serial.println();
  Serial.println("静音舱公网状态固件启动");

  initializeOled();
  updateOled(millis());

  RadarSerial.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  Serial.println("[雷达] UART 已启动：256000 8N1，RX=GPIO20，TX=GPIO21");
  delay(250);
  configureRadarForSilentBooth();

  WiFi.onEvent(reportWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiRetryMs = millis();
  Serial.println("[Wi-Fi] 正在连接...");
}

void loop() {
  readRadar();
  maintainWiFi();

  const uint32_t nowMs = millis();
  updateOled(nowMs);
  const bool heartbeatDue = nowMs - lastUploadAttemptMs >= UPLOAD_INTERVAL_MS;
  const bool stateChangeDue =
    statusDirty && nowMs - lastUploadAttemptMs >= STATE_CHANGE_UPLOAD_GAP_MS;
  if (heartbeatDue || stateChangeDue) {
    uploadStatus();
    // 从一次请求结束后再计时，避免慢网络失败后立即连续重试。
    lastUploadAttemptMs = millis();
  }

  delay(2);
}
