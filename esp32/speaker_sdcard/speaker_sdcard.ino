// Comments in English as you prefer
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

AudioGeneratorWAV *wav;
AudioFileSourceSD *file;
AudioOutputI2S *out;

#define SD_CS 5  // your SD card CS pin

void setup() {
  Serial.begin(115200);

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

  // Start playback
  if (!wav->begin(file, out)) {
    Serial.println("WAV begin failed");
    while (1);
  }
}

void loop() {
  if (wav->isRunning()) {
    if (!wav->loop()) wav->stop();
  } else {
    Serial.println("Playback finished.");
    delay(1000);
  }
}
