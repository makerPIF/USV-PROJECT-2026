/* =====================================================================
   ESC 가 생각하는 "정지"를 찾아내는 도구
   PUMPKIN IDEA FACTORY MAKERSPACE

   왜 필요한가
   -----------------------------------------------------------------
   GCS 에 좌/우현 펄스가 1000us 로 찍히는데도 모터가 돈다면, 보드는
   제대로 내보내고 있는데 ESC 가 그 1000us 를 "정지" 로 읽지 않는 것입니다.

   HobbyWing SeaKing V4 는 Running Mode 가 두 가지입니다.

     Forward and Reverse (양방향) : 가운데 1500us 가 정지
                                    1000us 는 "풀 후진" 입니다
     Forward Only       (전진전용) : 맨 아래 1000us 가 정지

   공장 설정이 어느 쪽인지, 보정이 되어 있는지는 눈으로 알 수 없습니다.
   그래서 이 스케치로 "직접 찾습니다". 추측하지 않고 측정합니다.

   준비
   -----------------------------------------------------------------
   1) ★★ 프로펠러를 반드시 빼세요. 예외 없습니다.
   2) ESC 신호선 흰색 -> ESP32 GPIO25 (아래 PIN_ESC 와 같게)
      ESC 신호선 검정 -> ESP32 GND          (반드시)
      ESC 신호선 빨강 -> 연결하지 않음 (뽑아서 절연)
      ESC 신호선 노랑 -> 연결하지 않음 (AUX, 조종기용)
   3) ESC 배터리는 아직 연결하지 마세요.
   4) ESP32 는 USB 로만 전원을 넣고, 시리얼 모니터 115200 을 엽니다.
      ※ 시리얼 모니터 오른쪽 아래 줄바꿈 설정은 아무거나 괜찮습니다.

   쓰는 법 — 세 단계
   -----------------------------------------------------------------
   [1단계] 이 스케치를 올리면 1500us 를 내보냅니다.
           그 상태에서 ESC 에 배터리를 연결하고 소리를 들으세요.

             짧은 삐 x N      = 셀 수 (3S 면 3번)
             이어서 길게 삐-- = ★ 지금 이 값이 정지입니다. 찾았습니다.
             길게 삐-- 없음   = 이 값은 정지가 아닙니다. 2단계로.

   [2단계] 길게 삐-- 가 안 들렸다면 값을 바꿔 가며 다시 시도합니다.
           z 를 눌러 1000 으로 바꾸고, ESC 배터리를 뺐다 다시 꽂으세요.
           ※ ESC 는 "전원이 들어오는 순간" 의 신호만 봅니다.
             그래서 값을 바꿀 때마다 배터리를 뺐다 꽂아야 합니다.

   [3단계] 정지를 찾았으면 t 를 눌러 모터가 도는지 확인합니다.
           정지값에서 +200us 까지 천천히 올렸다가 되돌아옵니다.

   누를 키
   -----------------------------------------------------------------
     n  1500us (양방향 ESC 의 정지)      z  1000us (전진전용 ESC 의 정지)
     +  +10us        -  -10us
     ]  +100us       [  -100us
     t  시험 : 지금 값에서 +200us 까지 올렸다 내림 (3초)
     s  자동 훑기 : 1000 -> 2000 을 50us 씩 올리며 모터를 관찰
     r  지금 값 보기
   ===================================================================== */

#include <ESP32Servo.h>

/* ===== 설정 ===== */
#define PIN_ESC   25      // ESC 신호선 흰색이 꽂힌 핀 (25 또는 26)

static const int US_MIN  = 1000;
static const int US_MAX  = 2000;
static const int US_BOOT = 1500;   // 처음 내보낼 값

Servo esc;
int   us = US_BOOT;

void showUs() {
  Serial.printf("  ▶ 지금 내보내는 값 : %d us", us);
  if      (us == 1500) Serial.print("   (양방향 ESC 라면 여기가 정지)");
  else if (us == 1000) Serial.print("   (전진전용 ESC 라면 여기가 정지)");
  Serial.println();
}

void setUs(int v) {
  us = constrain(v, US_MIN, US_MAX);
  esc.writeMicroseconds(us);
  showUs();
}

void help() {
  Serial.println();
  Serial.println(F("  -------------------------------------------------"));
  Serial.println(F("   n 1500us    z 1000us"));
  Serial.println(F("   + -10us 단위   ] [ 100us 단위"));
  Serial.println(F("   t 시험(+200us 까지)   s 자동 훑기   r 값 보기"));
  Serial.println(F("  -------------------------------------------------"));
}

/* 지금 값에서 +200us 까지 천천히 올렸다가 되돌립니다. */
void testRamp() {
  int base = us, top = min(base + 200, US_MAX);
  Serial.printf("\n  [시험] %d -> %d us 로 올립니다. 모터를 보세요.\n", base, top);
  for (int v = base; v <= top; v += 5) { esc.writeMicroseconds(v); delay(20); }
  delay(1200);
  Serial.println(F("  [시험] 되돌립니다."));
  for (int v = top; v >= base; v -= 5) { esc.writeMicroseconds(v); delay(20); }
  esc.writeMicroseconds(base);
  Serial.printf("  [시험] 끝. %d us 로 돌아왔습니다.\n", base);
  Serial.println(F("         -> 이 값에서 모터가 멈춰 있으면 여기가 정지입니다."));
}

/* 1000 -> 2000 을 50us 씩 올리며 관찰합니다. 아무 키나 누르면 멈춥니다. */
void sweep() {
  Serial.println(F("\n  [훑기] 1000 -> 2000 us 를 50us 씩 올립니다."));
  Serial.println(F("         ★ 모터가 '멈춰 있는' 구간의 값을 적어 두세요."));
  Serial.println(F("         아무 키나 누르면 중단합니다.\n"));
  while (Serial.available()) Serial.read();
  for (int v = US_MIN; v <= US_MAX; v += 50) {
    esc.writeMicroseconds(v);
    Serial.printf("     %4d us\n", v);
    for (int i = 0; i < 12; i++) {
      delay(100);
      if (Serial.available()) {
        while (Serial.available()) Serial.read();
        esc.writeMicroseconds(us);
        Serial.printf("  [훑기] 중단. %d us 로 되돌렸습니다.\n", us);
        return;
      }
    }
  }
  esc.writeMicroseconds(us);
  Serial.printf("  [훑기] 끝. %d us 로 되돌렸습니다.\n", us);
}

void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println();
  Serial.println(F("====================================================="));
  Serial.println(F("  ESC 정지 펄스 찾기"));
  Serial.printf ("  핀 GPIO%d\n", PIN_ESC);
  Serial.println(F("====================================================="));
  Serial.println(F("  ★★ 프로펠러를 뺐는지 다시 확인하세요."));
  Serial.println();

  esc.setPeriodHertz(50);
  esc.attach(PIN_ESC, US_MIN, US_MAX);
  esc.writeMicroseconds(us);

  Serial.printf("  %d us 를 내보내고 있습니다.\n", us);
  Serial.println(F("  지금 ESC 에 배터리를 연결하세요."));
  Serial.println();
  Serial.println(F("  들어야 할 소리"));
  Serial.println(F("    짧은 삐 x N    = 셀 수 (3S 면 3번)"));
  Serial.println(F("    이어서 길게 삐- = ★ 이 값이 정지입니다. 찾았습니다."));
  Serial.println(F("    길게 삐- 없음   = 이 값은 정지가 아닙니다."));
  Serial.println();
  Serial.println(F("  길게 삐- 가 안 들렸다면 :"));
  Serial.println(F("    z 를 눌러 1000us 로 바꾸고,"));
  Serial.println(F("    ESC 배터리를 뺐다가 다시 꽂으세요."));
  Serial.println(F("    (ESC 는 전원이 들어오는 순간의 신호만 봅니다)"));
  help();
}

void loop() {
  if (!Serial.available()) return;
  int k = Serial.read();

  switch (k) {
    case 'n': case 'N': setUs(1500); break;
    case 'z': case 'Z': setUs(1000); break;
    case '+': case '=': setUs(us + 10);  break;
    case '-': case '_': setUs(us - 10);  break;
    case ']': case '}': setUs(us + 100); break;
    case '[': case '{': setUs(us - 100); break;
    case 't': case 'T': testRamp(); break;
    case 's': case 'S': sweep();    break;
    case 'r': case 'R': showUs();   break;
    case '?': case 'h': help();     break;
    default: break;                      // 줄바꿈 문자 등은 무시
  }
}
