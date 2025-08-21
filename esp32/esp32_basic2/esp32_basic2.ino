#define LED 2
 
void setup() {
  // Set pin mode
  pinMode(LED,OUTPUT);
}
 
void loop() {
  delay(200);
  digitalWrite(LED,HIGH);
  delay(200);
  digitalWrite(LED,LOW);
}