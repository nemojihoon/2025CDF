#include <SPI.h>
#include <SD.h>

const int CS = 5;
File myFile;
int SerialNumber = 1;


void setup() {
  Serial.begin(115200);
  Serial.println("Initializing SD card...");
  Serial.println("\n==============================================");
  Serial.print("MOSI: ");
  Serial.println(MOSI);
  Serial.print("MISO: ");
  Serial.println(MISO);
  Serial.print("SCK: ");
  Serial.println(SCK);
  Serial.print("SS or CS: ");
  Serial.println(SS);
  Serial.println("==============================================\n");

  while (!SD.begin(CS)) {
    Serial.println("SD card initialization failed!");
    delay(1000);
  }

  delay(1000);
  Serial.println("SD card initialization done.");
  
}

void loop() {
 char formattedData[50];
  int randomNumber = random(100, 1000);
  sprintf(formattedData, "%lu-%d", SerialNumber, randomNumber);
  WriteFile("/NewFile.txt", formattedData);
  SerialNumber++;
  delay(1000);
}



void WriteFile(const char *path, const char *message) {
  myFile = SD.open(path, FILE_APPEND);
  if (myFile) {
    myFile.println(message);
    myFile.close();
  } else {
    Serial.println("error opening file");
  }
}
