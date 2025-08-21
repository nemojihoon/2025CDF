#include <WiFi.h>
#include <esp_now.h>


bool buttonDown = false;
bool ledOn = false;

void formatMacAddress(const uint8_t *macAddr, char *buffer, int maxLength) {
  snprintf(buffer, maxLength, "%02x:%02x:%02x:%02x:%02x:%02x",
           macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
}

// NEW: esp-idf 5.x style receive callback
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

  // simple command
  ledOn = (strcmp("on", buffer) == 0);
  digitalWrite(2, ledOn);
}

// NEW: esp-idf 5.x style send callback
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

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);   // clear previous state
  WiFi.setSleep(false);

  Serial.println("ESPNow Example");
  Serial.print("My MAC Address is: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() == ESP_OK) {
    Serial.println("ESPNow Init Success");
    esp_now_register_recv_cb(receiveCallback); // NEW signature
    esp_now_register_send_cb(sentCallback);    // NEW signature
  } else {
    Serial.println("ESPNow Init Failed");
    delay(3000);
    ESP.restart();
  }

  pinMode(0, INPUT_PULLUP);
  pinMode(2, OUTPUT);
}

void loop() {
  if (digitalRead(0)) {
    if (!buttonDown) {
      buttonDown = true;
      ledOn = !ledOn;
      digitalWrite(2, ledOn);
      broadcast(ledOn ? "on" : "off");
    }
    delay(500); // debounce
  } else {
    buttonDown = false;
  }
}
