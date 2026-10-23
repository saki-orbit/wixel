#include <WiFi.h>

#include "secrets.h"
#define SSID WIFI_SSID
#define PASSWORD WIFI_PASSWORD
uint32_t lastAttemptMs = 0;

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("[诊断] Wi-Fi 已关联，等待 IP");
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    Serial.print("[诊断] 已取得 IP: ");
    Serial.println(WiFi.localIP());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.print("[诊断] 断开原因码: ");
    Serial.println(info.wifi_sta_disconnected.reason);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(500);
  Serial.println("[诊断] 开始测试 Wi-Fi 连接");
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(SSID, PASSWORD);
  lastAttemptMs = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED && millis() - lastAttemptMs > 30000) {
    lastAttemptMs = millis();
    Serial.println("[诊断] 30 秒未连接，重新尝试");
    WiFi.disconnect(false);
    WiFi.begin(SSID, PASSWORD);
  }
  delay(100);
}
