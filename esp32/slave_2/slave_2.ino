#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <DFRobotDFPlayerMini.h>

// WiFiMulti WiFiMulti;
// const char* WIFI_SSID = "302-211";
// const char* WIFI_PASS = "";  // 필요 시 비번

// === add: hub index and app-level ACK state ===
#define HUB_IDX 1

// link-level send result flags (from send-callback)
volatile bool g_lastSendDone = false;
volatile bool g_lastSendOk   = false;

// app-level ACK waiting state
volatile uint16_t g_waitSeq = 0;
volatile bool     g_ackGot  = false;

uint16_t nextSeq() { static uint16_t s = 0; return ++s; }

// MAC table
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

//vibration detection
#define VIB_PIN        4       // 디지털 입력
#define VIB_DEBOUNCE_MS  1500 // 잡음 방지
volatile bool vibISRFlag = false;
volatile uint32_t vibLastMs = 0;

// game state
#define ID 2
volatile bool pendingStart = false;
volatile bool pendingStop  = false;
volatile int  pendingMode  = 0;
volatile int  pendingVol   = 0;
volatile bool isPlaying = false;
volatile bool isMe = false;
volatile int mode = 0;
volatile int volume = 0;
volatile int answer = 0;

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

// helper: add peer (broadcast or unicast)
static bool ensurePeer(const uint8_t addr[6]) {
  if (esp_now_is_peer_exist(addr)) return true;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, addr, 6);
  peerInfo.ifidx   = WIFI_IF_STA;   // use STA interface
  peerInfo.channel = 0;             // 0 = current channel; or set explicit WiFi.channel()
  peerInfo.encrypt = false;         // no LMK
  // peerInfo.lmk left zeroed
  
  
  return (esp_now_add_peer(&peerInfo) == ESP_OK);
}

void broadcast(const String &message) {
  // broadcast FF:FF:FF:FF:FF:FF
  const uint8_t broadcastAddress[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  if (!ensurePeer(broadcastAddress)) {
    Serial.println("Failed to add broadcast peer");
    return;
  }
  esp_err_t result = esp_now_send(broadcastAddress,
                                  (const uint8_t*)message.c_str(),
                                  message.length());
  if (result == ESP_OK) {
    Serial.println("Broadcast message success");
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
    Serial.println("Unknown error");
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

// === add: ensure peer then send with light retry ===
bool sendSafe(const uint8_t addr[6], const uint8_t* data, size_t len) {
  if (!esp_now_is_peer_exist(addr)) {
    if (!ensurePeer(addr)) {
      Serial.println("ensurePeer failed");
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

// === add: link-level check using send-callback ===
bool sendWithLinkRetry(const uint8_t addr[6], const String& s, int retries=1, uint32_t timeoutMs=30) {
  for (int t=0; t<=retries; ++t) {
    g_lastSendDone = false; g_lastSendOk = false;
    if (!sendSafe(addr, (const uint8_t*)s.c_str(), s.length())) {
      delay(2);
      continue;
    }
    uint32_t start = millis();
    while (!g_lastSendDone && (millis()-start) < timeoutMs) { delay(1); }
    if (g_lastSendDone && g_lastSendOk) return true;
    delay(2);
  }
  return false;
}

// === add: app-level ACK roundtrip ===
// kind: "CORRECT" / "FAIL" 등, payload: "KIND,ID,SEQ"
bool sendWithAppAck_ToHub(const char* kind, int nodeId, uint32_t timeoutMs=120, int retries=2) {
  uint16_t seq = nextSeq();
  char msg[40];
  snprintf(msg, sizeof(msg), "%s,%d,%u", kind, nodeId, (unsigned)seq);

  for (int t=0; t<=retries; ++t) {
    g_waitSeq = seq; g_ackGot = false;

    if (!sendWithLinkRetry(PEERS[HUB_IDX], String(msg), 1, 35)) {
      delay(3);
      continue;
    }

    uint32_t start = millis();
    while (!g_ackGot && (millis() - start) < timeoutMs) { delay(2); }
    if (g_ackGot) return true;     // app-level processed by HUB
    // timeout → retry
    delay(5);
  }
  return false;
}

// HUB-side receiveCallback (essential part)
void receiveCallback(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len <= 0) return;

  char buf[ESP_NOW_MAX_DATA_LEN+1];
  int n = min(ESP_NOW_MAX_DATA_LEN, len);
  strncpy(buf, (const char*)data, n); buf[n] = 0;

  // e.g., "CORRECT,ID,SEQ"
  if (strncmp(buf, "CORRECT,", 8) == 0) {
    // parse id and seq
    // format: CORRECT,<id>,<seq>
    char* p = buf + 8;
    int id  = atoi(p);
    char* comma = strchr(p, ',');
    uint16_t seq = 0;
    if (comma) seq = (uint16_t)atoi(comma+1);

    // 1) ACK back to the sender
    char ack[16];
    snprintf(ack, sizeof(ack), "ACK,%u", (unsigned)seq);
    sendWithLinkRetry(info->src_addr, String(ack), 1, 35);

    // 2) redistribute official CORRECT to all nodes (except hub)
    //    add small pacing to avoid queue congestion
    // for (int i=2; i<=4; ++i) {  // adjust indices to your nodes
    //   sendWithLinkRetry(PEERS[i], String("CORRECT"), 1, 35);
    //   delayMicroseconds(1500);
    // }

    // 3) local state: stop your own round, log, etc.
    pendingStop = true; isPlaying = false;
    return;
  }

  // optionally handle "FAIL,ID,SEQ"
  if (strncmp(buf, "FAIL,", 5) == 0) {
    // parse/use as needed, then ACK if you also want app-level confirmation
    char* p = buf + 5;
    char* comma = strchr(p, ',');
    uint16_t seq = 0;
    if (comma) seq = (uint16_t)atoi(comma+1);

    char ack[16];
    snprintf(ack, sizeof(ack), "ACK,%u", (unsigned)seq);
    sendWithLinkRetry(info->src_addr, String(ack), 1, 35);
    return;
  }

  isPlaying = true;
  char *token = strtok(buf, ",");
  int m=0, v=0, who=0;
  if (token) { m = atoi(token); token = strtok(NULL, ","); }
  if (token) { v = atoi(token); token = strtok(NULL, ","); }
  if (token) { who = atoi(token); }
  mode   = m;
  volume = v;
  isMe   = (ID == who);
  answer = who;
  pendingMode = mode;
  pendingVol  = volume;
  pendingStart = true;   // loop()에서 startRound 실행
  vibISRFlag = false;
  return;
}

// replace your sentCallback with this (common signature)
void sentCallback(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  g_lastSendOk   = (status == ESP_NOW_SEND_SUCCESS);
  g_lastSendDone = true;
  Serial.print("Last Packet Send Status: ");
  Serial.println(g_lastSendOk ? "Delivery Success" : "Delivery Fail");
}

void startRound(int mode, int volume) {
  switch(mode) {
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

void stopMode2() {
  neopixelOff();
  player.stop();
  trackNum = 0;
}

void startLoopTrack() {
  player.play(trackNum);
  Serial.printf("Looping track #%u\n", trackNum);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // // WiFi (STA) - AP 연결 (WebSocket용 + ESP-NOW 공존)
  // WiFi.mode(WIFI_STA);
  // WiFi.setSleep(false);
  // WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  // Serial.print("Connecting WiFi");
  // while (WiFiMulti.run() != WL_CONNECTED) {
  //   Serial.print(".");
  //   delay(500);
  // }

  // Wi‑Fi를 STA로 고정 (중요)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false); // 불필요한 AP 연결 시도 방지

  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  Serial.println("HUB MAC: " + WiFi.macAddress());

  // NeoPixel
  ring.begin();
  ring.setBrightness(80); // 필요 시 0~255 조절
  neopixelOff();

  // Vibration ISR
  pinMode(VIB_PIN, INPUT); // 모듈 DO가 기본 HIGH/LOW 출력
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
  if (pendingStop) {
    pendingStop = false;
    stopRound(mode);
  }
  if (pendingStart) {
    pendingStart = false;
    startRound(pendingMode, pendingVol);
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
    delay(150); // small settling delay

    if (isMe) {
      isMe = false;
      bool ok = sendWithAppAck_ToHub("CORRECT", ID, 120, 2);
      if (!ok) {
        Serial.println("Failed to deliver CORRECT to HUB (no ACK).");
      }

      pendingStop = true;
      isPlaying = false;

    } else {
      sendWithAppAck_ToHub("FAIL", ID, 80, 1); // optional
    }
  }
  delay(50);
}
