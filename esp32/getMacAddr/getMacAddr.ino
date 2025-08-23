#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);   // 반드시 모드 지정
  delay(100);

  Serial.print("STA MAC: ");
  Serial.println(WiFi.macAddress());

  WiFi.mode(WIFI_AP);
  delay(100);
  Serial.print("AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
}
void loop() {}
