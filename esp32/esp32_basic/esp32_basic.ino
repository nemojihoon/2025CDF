#define LED 2
 
void setup() {
  // Set pin mode
  Serial.begin(115200);
  pinMode(LED,OUTPUT);
}
 
void loop() {
  Serial.println("hello");
  delay(500);
  digitalWrite(LED,HIGH);
  delay(500);
  digitalWrite(LED,LOW);
}