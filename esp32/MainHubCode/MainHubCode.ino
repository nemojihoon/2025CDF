#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <DFRobotDFPlayerMini.h>

WiFiMulti WiFiMulti;
const char* WIFI_SSID = "302-211";
const char* WIFI_PASS = "";  // 필요 시 비번

WebSocketsServer webSocket(81);
uint8_t clientNum = 0;

// === HUB index (we decided [1] is Hub) ===
#define HUB_IDX 1

// link-level send result flags (from send-callback)
volatile bool g_lastSendDone = false;
volatile bool g_lastSendOk   = false;


//MAC table
const uint8_t PEERS[5][6] = {
  {0x00,0x00,0x00,0x00,0x00,0x00},          // [0] unused
  {0x3C,0x8A,0x1F,0x0B,0x91,0xC0},          // [1] 3C:8A:1F:0B:91:C0
  {0xF0,0x24,0xF9,0x45,0xF8,0xDC},          // [2] F0:24:F9:45:F8:DC
  {0x80,0xF3,0xDA,0xAC,0xE3,0xB4},          // [3] 80:F3:DA:AC:E3:B4
  {0x80,0xF3,0xDA,0xAD,0x0B,0x2C}           // [4] 80:F3:DA:AD:0B:2C
};

// neopixel
#define NEOPIXEL_PIN   16      // 데이터핀 (필요시 변경)
#define NUM_LEDS       12
Adafruit_NeoPixel ring(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

//vibration
#define VIB_PIN        4       // 디지털 입력
#define VIB_DEBOUNCE_MS  1500 // 잡음 방지
volatile bool vibISRFlag = false;
volatile uint32_t vibLastMs = 0;

// game state
#define ID 1
volatile bool pendingStop  = false;
volatile bool isPlaying = false;
volatile bool bcastAnswer = false;
bool isMe = false;
int mode = 0;
int volume = 0;
int answer = 0;
volatile int failCnt = 0;

// dfplayer
static const uint8_t TX_PIN = 26; // ESP32 TX to DFPlayer RX
static const uint8_t RX_PIN = 27; // ESP32 RX from DFPlayer TX

HardwareSerial mp3Serial(2);  // UART2
DFRobotDFPlayerMini player;

uint16_t trackNum = 0;
static const uint32_t kFinishDebounceMs = 150; // debounce for duplicate finish events
uint32_t lastFinishMs = 0;

//neopixel
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

// vibration detection
void IRAM_ATTR onVibrationISR() {
  uint32_t now = millis();
  if (now - vibLastMs < VIB_DEBOUNCE_MS) return;
  vibLastMs = now;
  vibISRFlag = true;
}

// espnow
void formatMacAddress(const uint8_t *macAddr, char *buffer, int maxLength) {
  snprintf(buffer, maxLength, "%02x:%02x:%02x:%02x:%02x:%02x",
           macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
}

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

// Call this in setup() after esp_now_init()
void registerAllPeers() {
  for (int i = 1; i <= 4; ++i) {     // [1]~[4]
    bool nonzero = false;
    for (int b = 0; b < 6; ++b) {
      if (PEERS[i][b] != 0x00) { nonzero = true; break; }
    }
    if (!nonzero) continue;
    ensurePeer(PEERS[i]);
  }
}

// 상태코드까지 출력하는 브로드캐스트
void broadcast(const String &message) {
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

void unicast(const uint8_t addr[6], const String& message) {
  // Guard: empty message ok but not recommended
  if (!addr) {
    Serial.println("Invalid MAC pointer");
    return;
  }

  // Make sure peer exists (channel/encryption handled inside ensurePeer)
  if (!ensurePeer(addr)) {
    Serial.println("Failed to add/find peer");
    return;
  }

  // Send the payload
  esp_err_t result = esp_now_send(addr,
                                  (const uint8_t*)message.c_str(),
                                  message.length());

  // Error handling consistent with broadcast()
  if (result == ESP_OK) {
    Serial.println("Unicast message success");
    Serial.println(message);
  } else if (result == ESP_ERR_ESPNOW_NOT_INIT) {
    Serial.println("ESPNOW not Init.");
  } else if (result == ESP_ERR_ESPNOW_ARG) {
    Serial.println("Invalid Argument");
  } else if (result == ESP_ERR_ESPNOW_INTERNAL) {
    Serial.println("Internal Error");
  } else if (result == ESP_ERR_ESPNOW_NO_MEM) {
    Serial.println("ESP_ERR_ESPNOW_NO_MEM");
  } else if (result == ESP_ERR_ESPNOW_NOT_FOUND) {
    Serial.println("Peer not found.");
  } else {
    Serial.printf("Unknown error (%d)\n", (int)result);
  }
}

// Ensure peer then send with light retry
bool sendSafe(const uint8_t addr[6], const uint8_t* data, size_t len) {
  if (!esp_now_is_peer_exist(addr)) {
    if (!ensurePeer(addr)) {
      Serial.println("[NOW] ensurePeer failed");
      return false;
    }
  }
  esp_err_t err = esp_now_send(addr, data, len);

  // quick retry for transient errors
  if (err == ESP_ERR_ESPNOW_NOT_FOUND) {
    if (ensurePeer(addr)) err = esp_now_send(addr, data, len);
  } else if (err == ESP_ERR_ESPNOW_NO_MEM || err == ESP_ERR_ESPNOW_INTERNAL) {
    delay(3);
    err = esp_now_send(addr, data, len);
  }
  return (err == ESP_OK);
}

bool sendWithLinkRetry(const uint8_t addr[6], const String& s, int retries=1, uint32_t timeoutMs=35) {
  for (int t=0; t<=retries; ++t) {
    g_lastSendDone = false; g_lastSendOk = false;
    if (!sendSafe(addr, (const uint8_t*)s.c_str(), s.length())) { delay(2); continue; }
    uint32_t start = millis();
    while (!g_lastSendDone && (millis()-start) < timeoutMs) { delay(1); }
    if (g_lastSendDone && g_lastSendOk) return true;
    delay(2);
  }
  return false;
}

void receiveCallback(const esp_now_recv_info_t *info, const uint8_t *data, int dataLen) {
  if (!info || !data || dataLen <= 0) return;

  char buffer[ESP_NOW_MAX_DATA_LEN + 1];
  int msgLen = min(ESP_NOW_MAX_DATA_LEN, dataLen);
  strncpy(buffer, (const char *)data, msgLen);
  buffer[msgLen] = 0;

  char macStr[18] = {0};
  formatMacAddress(info->src_addr, macStr, sizeof(macStr));
  Serial.printf("[NOW] RX from %s: %s\n", macStr, buffer);

  char* kind = strtok(buffer, ",");
  if (!kind) return;
  char* idStr  = strtok(NULL, ",");
  char* seqStr = strtok(NULL, ",");
  int whoId = idStr  ? atoi(idStr)  : 0;
  uint16_t seq = seqStr ? (uint16_t)atoi(seqStr) : 0;

  // Build ACK message back to sender
  char ackMsg[16];
  snprintf(ackMsg, sizeof(ackMsg), "ACK,%u", (unsigned)seq);

  if (strcmp(kind, "CORRECT") == 0) {
    // 1) ACK back to the sender (app-level confirmation)
    sendWithLinkRetry(info->src_addr, String(ackMsg), 1, 35);

    // 2) Stop local round and report to WebSocket with attempts
    pendingStop = true;
    isPlaying = false;

    // attempts = failCnt + 1 (failCnt == number of FAIL before the CORRECT)
    int attempts = failCnt + 1;
    failCnt = 0;
    String wsMsg = "CORRECT," + String(attempts);
    webSocket.sendTXT(clientNum, wsMsg);

    // 3) Redistribute official CORRECT to all nodes (except Hub)
    for (int i = 2; i <= 4; ++i) {
      sendWithLinkRetry(PEERS[i], String("CORRECT"), 1, 35);
      delayMicroseconds(1500); // short pacing
    }
    return;
  }

  if (strcmp(kind, "FAIL") == 0) {
    // 1) increase fail count
    failCnt++;
    Serial.printf("[NOW] FAIL count: %d\n", failCnt);

    // 2) ACK back to the sender
    sendWithLinkRetry(info->src_addr, String(ackMsg), 1, 35);
    return;
  }
}

// Replace sentCallback with this common signature:
void sentCallback(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  g_lastSendOk   = (status == ESP_NOW_SEND_SUCCESS);
  g_lastSendDone = true;
  Serial.print("[NOW] Last Packet Send Status: ");
  Serial.println(g_lastSendOk ? "Delivery Success" : "Delivery Fail");
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
      stopMode1();
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
void startMode1(int volume) {
  uint32_t c = randomColor();
  neopixelAll(c);
}

void startMode2(int volume) {
  if(isMe) {
    uint32_t c = randomColor();
    neopixelAll(c);
    player.volume(constrain((volume * 30) / 100, 0, 30));
    trackNum = 1;
    startLoopTrack();
  } else {
    player.volume(0);
  }
}

void stopMode1() {
  neopixelOff();
}

void stopMode2() {
  neopixelOff();
  player.stop();
  trackNum = 0;
}

void startLoopTrack() {
  player.play(trackNum);
  Serial.printf("Looping track #%u\n", trackNum);
}

// websocket
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  clientNum = num;
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

      char buf[64];  // 버퍼 크기는 상황에 맞게
      msg.toCharArray(buf, sizeof(buf));

      char *token = strtok(buf, ",");
      if (token != NULL) {
        mode = atoi(token);
        token = strtok(NULL, ",");
      }
      if (token != NULL) {
        volume = atoi(token);   
        token = strtok(NULL, ",");
      }
      if (token != NULL) {
        isMe = (ID == atoi(token));
        answer = atoi(token);        
      }
      Serial.printf("mode: %d\n", mode);
      Serial.printf("volume: %d\n", volume);
      Serial.printf("answer: %d\n", answer);

      isPlaying = true;
      bcastAnswer = true;
      startRound(mode, volume);
      break;
    }

  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

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

  // NeoPixel
  ring.begin();
  ring.setBrightness(80);
  neopixelOff();

  // Vibration ISR
  pinMode(VIB_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(VIB_PIN), onVibrationISR, RISING);

  // ESP-NOW 초기화 
  if (esp_now_init() == ESP_OK) {
    Serial.println("ESP-NOW Init Success");
    esp_now_register_send_cb(sentCallback);
    esp_now_register_recv_cb(receiveCallback);
  } else {
    Serial.println("ESP-NOW Init Failed. Rebooting...");
    delay(2000);
    ESP.restart();
  }
  registerAllPeers();

  // WebSocket
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);

  //dfplayer
  mp3Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println("Initializing DFPlayer Mini...");
  if (!player.begin(mp3Serial)) {
    Serial.println("DFPlayer init failed! Check wiring and SD card.");
    while (true) delay(100);
  }

  Serial.println("DFPlayer Mini is online.");
  player.volume(10);
}


void loop() {
  webSocket.loop();

  if (bcastAnswer) {
    bcastAnswer = false;
    String msg = String(mode) + "," + String(volume) + "," + String(answer);

    delay(500);
    for (int i = 2; i <= 2; i++) {
      sendWithLinkRetry(PEERS[i], msg, 1, 35);
      delayMicroseconds(1500);
    }
  }

  if (pendingStop) {
    pendingStop = false;
    stopRound(mode);
    String msg = "CORRECT," + String(failCnt+1);
    webSocket.sendTXT(clientNum, msg);
    failCnt = 0;
  }

  if (player.available() && isPlaying && trackNum != 0) {
    uint8_t type = player.readType();
    int value    = player.read();

    switch (type) {
      case DFPlayerPlayFinished: {
        uint32_t now = millis();
        if (now - lastFinishMs > kFinishDebounceMs) {
          Serial.printf("Track finished: %d -> restarting\n", value);
          lastFinishMs = now;
          delay(50);
          startLoopTrack();
        }
        break;
      }
      case DFPlayerError:
        Serial.printf("DFPlayer Error: %d\n", value);
        break;
      default:
        break;
    }
  }

  if (vibISRFlag && isPlaying) {
    vibISRFlag = false;
    delay(100);

    if (isMe) {
      pendingStop = true;
      isPlaying = false;

      int attempts = failCnt + 1;
      failCnt = 0;

      if(mode != 1) {
        for (int i = 2; i <= 4; ++i) {
          sendWithLinkRetry(PEERS[i], String("CORRECT"), 1, 35);
          delayMicroseconds(1500);
        }
      }
      

    } else {
      failCnt++;
      Serial.printf("[HUB] False tap, FAIL count=%d\n", failCnt);
    }
  }

  delay(50);
}

