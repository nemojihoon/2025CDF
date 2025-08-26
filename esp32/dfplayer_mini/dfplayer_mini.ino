#include <Arduino.h>
#include <DFRobotDFPlayerMini.h>

// === Pin mapping for ESP32 <-> DFPlayer ===
// DFPlayer RX  <- ESP32 TX_PIN
// DFPlayer TX  -> ESP32 RX_PIN
static const uint8_t TX_PIN = 26; // ESP32 TX to DFPlayer RX
static const uint8_t RX_PIN = 27; // ESP32 RX from DFPlayer TX

HardwareSerial mp3Serial(2);  // UART2
DFRobotDFPlayerMini player;

static const uint16_t kTrackToLoop = 1;  // loop this track
static const uint32_t kFinishDebounceMs = 150; // debounce for duplicate finish events
uint32_t lastFinishMs = 0;

void startLoopTrack() {
  // You can also try: player.loop(kTrackToLoop); // 일부 모듈/펌웨어에서 동작
  player.play(kTrackToLoop);
  Serial.printf("Looping track #%u\n", kTrackToLoop);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  mp3Serial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  Serial.println("Initializing DFPlayer Mini...");
  if (!player.begin(mp3Serial)) {
    Serial.println("DFPlayer init failed! Check wiring and SD card.");
    while (true) delay(100);
  }

  Serial.println("DFPlayer Mini is online.");
  player.volume(10);   // 0~30

  // start first play
  startLoopTrack();
}

void loop() {
  if (player.available()) {
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

  delay(5);
}
