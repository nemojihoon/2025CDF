/*
 * Tapping Practice (Mode1) with WS2812B & SW-420
 * - WebSocket control
 * - Random solid color per round
 * - Vibration detection (SW-420, digital)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebSocketsServer.h>
#include <Adafruit_NeoPixel.h>
#include <esp_now.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

#define NEOPIXEL_PIN   16      // 데이터핀 (필요시 변경)
#define NUM_LEDS       12
Adafruit_NeoPixel ring(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

#define VIB_PIN        4       // 디지털 입력 (DO)
#define VIB_DEBOUNCE_MS  1000; // 잡음 방지
volatile bool vibISRFlag = false;
volatile uint32_t vibLastMs = 0;

#define ID 2
bool isME = false;
int mode = 0;

#define SD_CS 5  // your SD card CS pin

AudioGeneratorWAV *wav;
AudioFileSourceSD *file;
AudioOutputI2S *out;

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

  if(strcmp("stop", buffer) == 0) {
    stopRound(mode);
  } else {
    char *token = strtok(buffer, ",");
    if (token != NULL) {
      modeNum = atoi(token);
      token = strtok(NULL, ",");
    }
    if (token != NULL) {
      volume = atoi(token);   
      token = strtok(NULL, ",");
    }
    if (token != NULL) {
      isMe = (ID == atio(token));
    }
    startRound(mode, volume);
  }
}

void sentCallback(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  // NOTE: wifi_tx_info_t may not expose dest MAC the same way as before;
  // print status only (or inspect fields if needed).
  Serial.print("Last Packet Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
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
  wav->begin(file, out);
}

void stopMode2() {
  neopixelOff();
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

  // Init SD
  if (!SD.begin(SD_CS)) {
    Serial.println("SD init failed!");
    while (1);
  }
  Serial.println("SD init OK.");

  // Setup audio output to internal DAC (GPIO25)
  out = new AudioOutputI2S(
    0,                                 // I2S 포트 번호(대부분 0)
    AudioOutputI2S::INTERNAL_DAC,      // <- 여기!
    8,                                 // DMA 버퍼 개수(기본값 OK)
    AudioOutputI2S::APLL_DISABLE
  );

  // Open WAV file from SD
  file = new AudioFileSourceSD("/sound.wav");
  wav = new AudioGeneratorWAV();

  // 난수 시드
  randomSeed(esp_random());
}

void loop() {
  // 진동 발생 처리 (ISR 플래그 폴링)
  if(!wav->loop() && isMe) {
    wav->begin();
  }
  if (vibISRFlag && isMe) {
    vibISRFlag = false;
    isMe = false;
    wav->stop();

    broadcast("stop");
    neopixelAll(ring.Color(0,0,0));
    delay(500);
    stopRound(mode);
  }
}
