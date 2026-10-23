#include <Wire.h>
#include <U8g2lib.h>
#include <string.h>

// 独立的屏幕演示，不包含 Wi-Fi、雷达或云端密钥。
constexpr int OLED_SDA_PIN = 5;
constexpr int OLED_SCL_PIN = 6;
constexpr uint32_t OLED_REFRESH_MS = 80;
constexpr uint32_t OLED_WHISPER_DELAY_MS = 1500;
constexpr uint32_t OLED_WHISPER_PERIOD_MS = 6000;
constexpr uint32_t OLED_WHISPER_DURATION_MS = 1800;

U8G2_SH1106_128X32_VISIONOX_F_HW_I2C oled(
  U8G2_R0, U8X8_PIN_NONE, OLED_SCL_PIN, OLED_SDA_PIN
);
bool oledReady = false;
uint32_t lastOledRefreshMs = 0;
uint32_t faceTransitionMs = 0;
uint32_t faceStateSinceMs = 0;

enum FaceState : uint8_t { FACE_SCANNING, FACE_EMPTY, FACE_OCCUPIED, FACE_RADAR_ERROR };
FaceState faceTarget = FACE_SCANNING;

uint32_t demoStartedMs = 0;

FaceState currentFaceState(uint32_t nowMs) {
  // 每种表情保留 5 秒，方便观察小字出现和消失。
  if (demoStartedMs == 0) return FACE_SCANNING;
  switch (((nowMs - demoStartedMs) / 5000) % 4) {
    case 0: return FACE_SCANNING;
    case 1: return FACE_EMPTY;
    case 2: return FACE_OCCUPIED;
    default: return FACE_RADAR_ERROR;
  }
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

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(500);
  initializeOled();
  demoStartedMs = millis();
  Serial.println("[OLED 演示] 四种表情各 5 秒；小字随后短暂出现");
}

void loop() {
  updateOled(millis());
  delay(5);
}
