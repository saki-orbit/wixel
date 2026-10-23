#include <WiFi.h>

constexpr const char* TARGET_SSID = "iPhone";

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(500);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.println("[诊断] 仅检查 ESP32 能否看到目标热点");
}

void loop() {
  const int count = WiFi.scanNetworks(false, true);
  bool found = false;
  if (count >= 0) {
    for (int i = 0; i < count; ++i) {
      const String name = WiFi.SSID(i);
      String folded = name;
      folded.toLowerCase();
      if (folded.indexOf("phone") >= 0) {
        Serial.printf("[诊断] 近似名称: <%s>，信号 %d dBm\n",
                      name.c_str(), WiFi.RSSI(i));
      }
      if (name == TARGET_SSID) {
        found = true;
        Serial.printf("[诊断] 发现 iPhone 热点；信号 %d dBm，信道 %d\n",
                      WiFi.RSSI(i), WiFi.channel(i));
        break;
      }
    }
  }
  if (!found) Serial.printf("[诊断] 未发现 iPhone 热点；扫描结果 %d\n", count);
  WiFi.scanDelete();
  delay(8000);
}
