/*
 * Main HUB (WebSocket + NeoPixel + Vibration ISR) + ESP-NOW (send only)
 * - Web에서 MODE2/3/4 시작 신호를 받으면
 *   서브들에게 "모드수,음량,임의정답(1~4)" 형태의 CSV를 ESP-NOW 브로드캐스트로 발송
 * - MODE1은 발송하지 않음
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>

///////////////////////
// === WiFi ===
///////////////////////
WiFiMulti WiFiMulti;
// TODO: 바꿔 넣으세요
const char* WIFI_SSID = "302-211";
const char* WIFI_PASS = "";  // 필요 시 비번

///////////////////////
// === WebSocket ===
///////////////////////
WebSocketsServer webSocket(81);

///////////////////////
// === NeoPixel ===
///////////////////////
#define NEOPIXEL_PIN   5
#define NUM_LEDS       12
Adafruit_NeoPixel ring(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

///////////////////////
// === Vibration (SW-420) ===
///////////////////////
#define VIB_PIN        4       // 디지털 입력 (DO)
volatile bool vibISRFlag = false;
volatile uint32_t vibLastMs = 0;
const uint32_t VIB_DEBOUNCE_MS = 200; // 잡음 방지

void IRAM_ATTR onVibrationISR() {
  uint32_t now = millis();
  if (now - vibLastMs < VIB_DEBOUNCE_MS) return;
  vibLastMs = now;
  vibISRFlag = true;
}

///////////////////////
// === Mode1 State ===
///////////////////////

#define ID 1
bool isME = false;
int mode = 0;

bool     mode1Active = false;
uint16_t mode1TotalRounds = 0;
uint16_t mode1CurrentRound = 0;
bool     mode1ReportedThisRound = false; // 이 라운드에서 정답 보고 했는지

// 유틸: 랜덤 색 생성
uint32_t randomColor() {
  uint8_t r = random(40, 256);
  uint8_t g = random(40, 256);
  uint8_t b = random(40, 256);
  return ring.Color(r, g, b);
}

void neopixelAll(uint32_t c) {
  for (int i = 0; i < NUM_LEDS; i++) ring.setPixelColor(i, c);
  ring.show();
}

void neopixelOff() {
  neopixelAll(ring.Color(0,0,0));
}

void startRound(int mode, int volume) {
  switch(mode) {
    case 1:
      startMode1(volume);
      break;
    case 2:
      startMode2(volume);
      break;
    // case 3:
    //   startMode3(volume);
    //   break;
    // case 4:
    //   startMode4(volume);
    //   break;
  }
}

void stopRound(int mode) {
  switch(mode) {
    case 1:
      stopMode1()
      break;
    case 2:
      stopMode2();
      break;
    // case 3:
    //   stopMode3();
    //   break;
    // case 4:
    //   stopMode4();
    //   break;
  }
}

// start
void startMode2(int volume) {
  uint32_t c = randomColor();
  neopixelAll(c);
  wav->begin(file, out);
}

void stopMode2() {
  neopixelOff();
}



// 라운드 시작: 색 점등 + 클라에 공지
void mode1StartRound(uint8_t clientNum) {
  if (!mode1Active) return;
  if (mode1CurrentRound > mode1TotalRounds) return;

  mode1ReportedThisRound = false;
  uint32_t c = randomColor();
  neopixelAll(c);
}

void mode1Stop(uint8_t clientNum, bool sendDone = true){
  mode1Active = false;
  mode1TotalRounds = 0;
  mode1CurrentRound = 0;
  mode1ReportedThisRound = false;
  neopixelOff();
  if (sendDone) {
    webSocket.sendTXT(clientNum, "MODE1_DONE");
  }
}

///////////////////////
// === Helpers ===
///////////////////////

// key=value 파싱 (구분자 '|')
int getIntParam(const String& msg, const String& key, int defVal) {
  int pos = msg.indexOf(key + "=");
  if (pos < 0) return defVal;
  int end = msg.indexOf("|", pos);
  String v = (end < 0) ? msg.substring(pos + key.length() + 1)
                       : msg.substring(pos + key.length() + 1, end);
  return v.toInt();
}

///////////////////////
// === ESP-NOW ===
///////////////////////

// 브로드캐스트 피어 등록기
static bool ensurePeer(const uint8_t addr[6]) {
  if (esp_now_is_peer_exist(addr)) return true;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, addr, 6);
  peerInfo.ifidx   = WIFI_IF_STA;   // STA 인터페이스
  peerInfo.channel = 0;             // 0 = 현재 채널
  peerInfo.encrypt = false;         // LMK 미사용
  return (esp_now_add_peer(&peerInfo) == ESP_OK);
}

// 상태코드까지 출력하는 브로드캐스트
void nowBroadcast(const String &message) {
  const uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  if (!ensurePeer(bcast)) {
    Serial.println("[NOW] Failed to add broadcast peer");
    return;
  }
  esp_err_t result = esp_now_send(bcast, (const uint8_t*)message.c_str(), message.length());
  if (result == ESP_OK) {
    Serial.printf("[NOW] Broadcast OK: %s\n", message.c_str());
  } else if (result == ESP_ERR_ESPNOW_NOT_INIT) {
    Serial.println("[NOW] ESPNOW not Init.");
  } else if (result == ESP_ERR_ESPNOW_ARG) {
    Serial.println("[NOW] Invalid Argument");
  } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
    Serial.println("[NOW] Internal Error");
  } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
    Serial.println("[NOW] ESP_ERR_ESPNOW_NO_MEM");
  } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
    Serial.println("[NOW] Peer not found.");
  } else {
    Serial.println("[NOW] Unknown error");
  }
}

void receiveCallback(const esp_now_recv_info_t *info, const uint8_t *data, int dataLen) {
  // guard
  if (!info || !data || dataLen <= 0) return;

  // clamp length and ensure NUL
  char buffer[ESP_NOW_MAX_DATA_LEN + 1];
  int msgLen = min(ESP_NOW_MAX_DATA_LEN, dataLen);
  strncpy(buffer, (const char *)data, msgLen);
  buffer[msgLen] = 0;

  // format source MAC (info->src_addr)
  char macStr[18] = {0};
  formatMacAddress(info->src_addr, macStr, sizeof(macStr));

  Serial.printf("Received message from: %s - %s\n", macStr, buffer);

  if(strcmp("stop", buffer) == 0) {
    stopRound(mode);
  }
}

// (옵션) 송신 완료 콜백: 성공/실패 로그만
void sentCallback(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("[NOW] Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
}

///////////////////////
// === WebSocket 이벤트 ===
///////////////////////
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%u] Disconnected\n", num);
      break;

    case WStype_CONNECTED: {
      IPAddress ip = webSocket.remoteIP(num);
      Serial.printf("[%u] Connected from %s\n", num, ip.toString().c_str());
      webSocket.sendTXT(num, "CONNECTED");
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload);
      msg.trim();
      Serial.printf("[%u] RX: %s\n", num, msg.c_str());

      // ===== MODE1: 발송하지 않음 (기존 동작 유지) =====
      if (msg.startsWith("MODE1_START")) {
        int reps = getIntParam(msg, "reps", 1);
        mode1Active = true;
        mode1TotalRounds = (reps <= 0 ? 1 : reps);
        mode1CurrentRound = 1;
        mode1ReportedThisRound = false;
        webSocket.sendTXT(num, "MODE1_ACK|reps=" + String(mode1TotalRounds));
        mode1StartRound(num);
        break;
      }
      if (msg == "MODE1_NEXT") {
        if (mode1Active) {
          mode1CurrentRound++;
          if (mode1CurrentRound <= mode1TotalRounds) {
            mode1StartRound(num);
          } else {
            mode1Stop(num, true);
          }
        }
        break;
      }
      if (msg == "MODE1_STOP") {
        mode1Stop(num, true);
        break;
      }

      // ===== MODE2/3/4: 웹에서 받은 파라미터 → 서브로 CSV 발송 =====
      if (msg.startsWith("MODE2_RUN")) {
        int rounds     = getIntParam(msg, "rounds",     5);
        int brightness = getIntParam(msg, "brightness", 70);
        int volume     = getIntParam(msg, "volume",     80);
        Serial.printf("[MODE2_RUN] rounds=%d, brightness=%d, volume=%d\n", rounds, brightness, volume);

        webSocket.sendTXT(num, "MODE2_ACK");
        break;
      }

      if (msg.startsWith("MODE3_RUN")) {
        int rounds = getIntParam(msg, "rounds", 7);
        int volume = getIntParam(msg, "volume", 80);
        Serial.printf("[MODE3_RUN] rounds=%d, volume=%d\n", rounds, volume);

        webSocket.sendTXT(num, "MODE3_ACK");
        break;
      }

      if (msg.startsWith("MODE4_RUN")) {
        int rounds = getIntParam(msg, "rounds", 5);
        int volume = getIntParam(msg, "volume", 80);
        Serial.printf("[MODE4_RUN] rounds=%d, volume=%d\n", rounds, volume);

        webSocket.sendTXT(num, "MODE4_ACK");
        break;
      }

      // 디버그: LED 제어
      if (msg == "LED_ON")  { neopixelAll(ring.Color(255,255,255)); webSocket.sendTXT(num, "LED_STATE:1"); break; }
      if (msg == "LED_OFF") { neopixelOff();                        webSocket.sendTXT(num, "LED_STATE:0"); break; }

      // 알 수 없는 메시지
      webSocket.sendTXT(num, "UNKNOWN_CMD");
      break;
    }

    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // NeoPixel
  ring.begin();
  ring.setBrightness(80);
  neopixelOff();

  // Vibration ISR
  pinMode(VIB_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(VIB_PIN), onVibrationISR, RISING);

  // WiFi (STA) - AP 연결 (WebSocket용 + ESP-NOW 공존)
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  Serial.println("HUB MAC: " + WiFi.macAddress());

  // ESP-NOW 초기화 
  if (esp_now_init() == ESP_OK) {
    Serial.println("ESP-NOW Init Success");
    esp_now_register_send_cb(sentCallback);  // 수신 콜백 등록 안 함 (요청사항)
  } else {
    Serial.println("ESP-NOW Init Failed. Rebooting...");
    delay(2000);
    ESP.restart();
  }

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  // 난수 시드
  randomSeed(esp_random());
}

void loop() {
  webSocket.loop();

  // (참고) MODE1의 진동 발생 처리 (현 단계 유지)
  if (vibISRFlag) {
    vibISRFlag = false;
    if (mode1Active && !mode1ReportedThisRound) {
      mode1ReportedThisRound = true;
      String msg = "MODE1_CORRECT|round=" + String(mode1CurrentRound);
      webSocket.broadcastTXT(msg);

      neopixelAll(ring.Color(0,0,0));
      delay(500);
    }
  }
}
