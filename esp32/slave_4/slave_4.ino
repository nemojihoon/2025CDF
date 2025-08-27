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
#define VIB_DEBOUNCE_MS  2000 // 잡음 방지
volatile bool vibISRFlag = false;
volatile uint32_t vibLastMs = 0;

// game state
#define ID 4
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

uint16_t trackNum = 1;
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

  if (strcmp("CORRECT", buffer) == 0) {
    pendingStop = true;
    isPlaying = false;
  } else if(strcmp("fail", buffer) == 0) {
  } else {
    isPlaying = true;
    char *token = strtok(buffer, ",");
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
  }
}

void sentCallback(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // NOTE: wifi_tx_info_t may not expose dest MAC the same way as before;
  // print status only (or inspect fields if needed).
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
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
    delay(100);
    if(isMe) {
      isMe = false;
      pendingStop = true;
      isPlaying = false;
      for(int i = 1; i <= 4; i++) {
        if(i == ID) continue;
        unicast(PEERS[i], "CORRECT");
      }
    } else {
      unicast(PEERS[1], "fail");
    }
  }
  delay(50);
}
