#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
// #include <FS.h>
// #include <SD.h>
// #include <SPI.h>
// #include "AudioFileSourceSD.h"
// #include "AudioGeneratorWAV.h"
// #include "AudioOutputI2S.h"

#define NEOPIXEL_PIN   16      // 데이터핀 (필요시 변경)
#define NUM_LEDS       12
Adafruit_NeoPixel ring(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

#define VIB_PIN        4       // 디지털 입력
#define VIB_DEBOUNCE_MS  1000 // 잡음 방지
volatile bool vibISRFlag = false;
volatile uint32_t vibLastMs = 0;

volatile bool pendingStart = false;
volatile bool pendingStop  = false;
volatile int  pendingMode  = 0;
volatile int  pendingVol   = 0;
volatile bool isPlaying = false;

#define ID 2
bool isMe = false;
int mode = 0;
int volume = 0;

#define SD_CS 5  // your SD card CS pin

// AudioGeneratorWAV *wav;
// AudioFileSourceSD *file;
// AudioOutputI2S *out;
const int buzzerPin = 25;

const int c = 261;
const int d = 294;
const int e = 329;
const int f = 349;
const int g = 391;
const int gS = 415;
const int a = 440;
const int aS = 455;
const int b = 466;
const int cH = 523;
const int cSH = 554;
const int dH = 587;
const int dSH = 622;
const int eH = 659;
const int fH = 698;
const int fSH = 740;
const int gH = 784;
const int gSH = 830;
const int aH = 880;

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

  if (strcmp("success", buffer) == 0) {
    pendingStop = true;
    isPlaying = false;
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
    pendingMode = mode;
    pendingVol  = volume;
    pendingStart = true;   // loop()에서 startRound 실행
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
  uint32_t c = randomColor();
  neopixelAll(c);

  firstSection();
  // if (!wav || !file || !out) return;
  // if (!wav->isRunning()) {
  //   delete file;
  //   file = new AudioFileSourceSD("/sound.wav");
  //   wav->begin(file, out);
  // }
}

void stopMode2() {
  neopixelOff();
  // if(wav && wav->isRunning()) {
  //   wav->stop();
  // }
  noTone(buzzerPin);
}


void beep(int note, int duration)
{
  tone(buzzerPin, note, duration);
  noTone(buzzerPin);
  delay(50);
}
 

//첫번째 연주에 관한 섹션
void firstSection()
{
  beep(a, 500);
  beep(a, 500);    
  beep(a, 500);
  beep(f, 350);
  beep(cH, 150);  
  beep(a, 500);
  beep(f, 350);
  beep(cH, 150);
  beep(a, 650);
 
  delay(500);
 
  beep(eH, 500);
  beep(eH, 500);
  beep(eH, 500);  
  beep(fH, 350);
  beep(cH, 150);
  beep(gS, 500);
  beep(f, 350);
  beep(cH, 150);
  beep(a, 650);
 
  delay(500);
}

void setup() {
  Serial.begin(115200);
  delay(100);

  // NeoPixel
  ring.begin();
  ring.setBrightness(80); // 필요 시 0~255 조절
  neopixelOff();

  // Vibration
  pinMode(VIB_PIN, INPUT); // 모듈 DO가 기본 HIGH/LOW 출력
  attachInterrupt(digitalPinToInterrupt(VIB_PIN), onVibrationISR, RISING);

  // Wi‑Fi를 STA로 고정 (중요)
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false); // 불필요한 AP 연결 시도 방지
  
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

  // // Init SD
  // if (!SD.begin(SD_CS)) {
  //   Serial.println("SD init failed!");
  //   while (1);
  // }
  // Serial.println("SD init OK.");

  // // Setup audio output to internal DAC (GPIO25)
  // out = new AudioOutputI2S(
  //   0,                                 // I2S 포트 번호(대부분 0)
  //   AudioOutputI2S::INTERNAL_DAC,      // <- 여기!
  //   8,                                 // DMA 버퍼 개수(기본값 OK)
  //   AudioOutputI2S::APLL_DISABLE
  // );

  // // Open WAV file from SD
  // file = new AudioFileSourceSD("/sound.wav");
  // wav = new AudioGeneratorWAV();

  // 난수 시드
  randomSeed(esp_random());
}

void loop() {
  // 진동 발생 처리 (ISR 플래그 폴링)
  // if (isMe) {
  //   if (!wav->isRunning()) {
  //     delete file;
  //     file = new AudioFileSourceSD("/sound.wav");
  //     wav->begin(file, out);
  //   } else {
  //     if (!wav->loop()) {
  //       wav->stop();
  //     }
  //   }
  // }
  // if(isMe && mode == 2) {
  //   firstSection(); 
  // }
  if (pendingStop) {
    pendingStop = false;
    stopRound(mode);
  }
  if (pendingStart) {
    pendingStart = false;
    startRound(pendingMode, pendingVol);
  }
  if (vibISRFlag && isPlaying) {
    vibISRFlag = false;
    if(isMe) {
      isMe = false;
      broadcast("success");
      stopRound(mode);
    } else {
      broadcast("fail");    
    }
  }
}
