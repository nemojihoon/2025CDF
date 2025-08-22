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
//피아노를 배우신분 음악을 배우셨다면 코드에 대해서 각각의 값을 할당 시켜 줍니다.

const int buzzerPin = 25;
int counter = 0;
 
void setup()
{
  pinMode(buzzerPin, OUTPUT); //부저핀을 출력으로 설정
}
 
void loop()
{
  firstSection();     //첫번째 섹션을 연주합니다.
  secondSection();    //두번째 섹션을 연주합니다.
 
  //변주 1
  beep(f, 250);  
  beep(gS, 500);  
  beep(f, 350);  
  beep(a, 125);
  beep(cH, 500);
  beep(a, 375);  
  beep(cH, 125);
  beep(eH, 650);
  delay(500);
 
  
  secondSection(); //2번째 섹션을 반복
 
  //변주 2
  beep(f, 250);  
  beep(gS, 500);  
  beep(f, 375);  
  beep(cH, 125);
  beep(a, 500);  
  beep(f, 375);  
  beep(cH, 125);
  beep(a, 650);  
  delay(650);
}
 
//소리를 설정하는 부분입니다. 스피커 모듈의 핀을 설정하는 부분입니다.
void beep(int note, int duration)
{
  //스피커 핀의 설정을 정의 합니다.
  tone(buzzerPin, note, duration);
 
  //스피커 핀을 멈춥니다.
  noTone(buzzerPin);
 
  delay(50);
 
  //카운터를 증가 시킵니다.
  counter++;
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
 

// 두번째 연주에 관한 섹션
void secondSection()
{
  beep(aH, 500);
  beep(a, 300);
  beep(a, 150);
  beep(aH, 500);
  beep(gSH, 325);
  beep(gH, 175);
  beep(fSH, 125);
  beep(fH, 125);    
  beep(fSH, 250);
 
  delay(325);
 
  beep(aS, 250);
  beep(dSH, 500);
  beep(dH, 325);  
  beep(cSH, 175);  
  beep(cH, 125);  
  beep(b, 125);  
  beep(cH, 250);  
 
  delay(350);
}
