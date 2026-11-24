/* =====================================================================
   ESC 스로틀 범위 보정 스케치
   PUMPKIN IDEA FACTORY MAKERSPACE

   왜 필요한가
   -----------------------------------------------------------------
   HobbyWing SeaKing V4 같은 보트용 ESC 는 "조종기의 최대/최소가 몇
   마이크로초인지" 를 스스로 배웁니다. 보통은 조종기 스틱으로 가르치지만,
   이 배에는 조종기가 없습니다. 그래서 ESP32 가 대신 그 신호를 내보냅니다.

   ※ 새 ESC 는 처음 한 번 반드시 해야 합니다. 보정하지 않으면
      "삐빅- 삐빅-" 경고음이 나고 모터가 돌지 않습니다.

   준비
   -----------------------------------------------------------------
   1) ★ 프로펠러를 반드시 빼 두세요. 예외 없습니다.
   2) ESC 신호선 흰색 -> ESP32 GPIO25 (또는 26)
      ESC 신호선 검정 -> ESP32 GND          (반드시 연결)
      ESC 신호선 빨강 -> 연결하지 않음 (뽑아서 절연)
   3) ESC 배터리는 아직 연결하지 마세요.
   4) ESP32 는 USB 로만 전원을 넣습니다.
   5) 시리얼 모니터 115200 으로 열어 두세요.

   ESC 한 개씩 따로 합니다. 좌현을 끝내고 우현을 하세요.
   ===================================================================== */

#include <ESP32Servo.h>

/* ===== 설정 ===== */
#define PIN_ESC      25      // 보정할 ESC 가 꽂힌 핀 (25 또는 26)

//  1 = 전진 전용으로 보정 (full-range)   : 최저 펄스(1000us)가 "정지"
//  0 = 양방향으로 보정   (half-range)   : 가운데 펄스(1500us)가 "정지"
//
//  ★ 이 값은 메인 펌웨어의 ESC_FORWARD_ONLY 와 반드시 같아야 합니다.
//    다르면 ESC 에게 가르친 "정지" 와 펌웨어가 보내는 "정지" 가 어긋나,
//    멈추라는 신호가 전속이 됩니다.
//
//  ※ 2026-11 측정 : 이 배의 SeaKing V4 30A 는 1500us 에서 정지했습니다.
//    (Running Mode = Forward and Reverse) 그래서 0 입니다.
#define FORWARD_ONLY 0

static const int PULSE_MAX  = 2000;   // 최대 전진
static const int PULSE_MIN  = 1000;   // 최저
static const int PULSE_MID  = 1500;   // 가운데

#if FORWARD_ONLY
static const int PULSE_STOP = PULSE_MIN;
#else
static const int PULSE_STOP = PULSE_MID;
#endif

Servo esc;

void banner() {
  Serial.println();
  Serial.println(F("====================================================="));
  Serial.println(F("  ESC 스로틀 범위 보정"));
  Serial.printf ("  핀 GPIO%d  ·  모드 %s\n",
                 PIN_ESC, FORWARD_ONLY ? "전진 전용(full-range)"
                                       : "양방향(half-range)");
  Serial.printf ("  정지 펄스 = %d us\n", PULSE_STOP);
  Serial.println(F("====================================================="));
  Serial.println(F("  ★ 프로펠러를 뺐는지 다시 확인하세요."));
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(600);
  banner();

  esc.setPeriodHertz(50);
  esc.attach(PIN_ESC, PULSE_MIN, PULSE_MAX);

  // --- 1단계 : 최대 신호를 먼저 내보냅니다 -------------------------
  esc.writeMicroseconds(PULSE_MAX);
  Serial.printf("[1] 최대 신호 %d us 출력 중입니다.\n", PULSE_MAX);
  Serial.println(F("    지금 ESC 에 배터리를 연결하세요."));
  Serial.println(F("    -> 셀 수만큼 '삐-' 소리가 난 뒤"));
  Serial.println(F("    -> 2초쯤 지나 '삐- 삐-' 두 번 울리면 최대값을 외운 것입니다."));
  Serial.println();
  Serial.println(F("    두 번 울리는 것을 확인했으면 아무 키나 누르고 Enter."));

  while (!Serial.available()) { delay(50); }
  while (Serial.available()) Serial.read();

  // --- 2단계 : 정지 신호 ------------------------------------------
  esc.writeMicroseconds(PULSE_STOP);
  Serial.println();
  Serial.printf("[2] 정지 신호 %d us 출력 중입니다.\n", PULSE_STOP);
  Serial.println(F("    -> 길게 '삐———' 한 번 울리면 보정이 끝난 것입니다."));
  Serial.println();
  delay(4000);

  Serial.println(F("====================================================="));
  Serial.println(F("  보정 완료"));
  Serial.println(F("====================================================="));
  Serial.println(F("  이제 모터가 도는지 확인합니다."));
  Serial.println(F("  시리얼 창에 숫자를 넣고 Enter 를 치세요:"));
  Serial.println(F("     0   정지"));
  Serial.println(F("     1   아주 약하게 (10%)"));
  Serial.println(F("     2   약하게 (20%)"));
  Serial.println(F("     3   보통 (30%)"));
  Serial.println();
  Serial.println(F("  ★ 프로펠러를 뺀 상태에서만 하세요."));
  Serial.println(F("  ★ 모터가 반대로 돌면 모터 3선 중 아무 두 개를 바꿔 끼웁니다."));
  Serial.println();
}

void loop() {
  if (!Serial.available()) return;

  int k = Serial.read();
  while (Serial.available()) Serial.read();      // 나머지 버리기

  int pct = -1;
  if      (k == '0') pct = 0;
  else if (k == '1') pct = 10;
  else if (k == '2') pct = 20;
  else if (k == '3') pct = 30;
  else return;

  int us = PULSE_STOP + (PULSE_MAX - PULSE_STOP) * pct / 100;
  esc.writeMicroseconds(us);
  Serial.printf("  출력 %d%%  ->  %d us\n", pct, us);

  if (pct > 0) {
    delay(3000);                                  // 3초만 돌리고
    esc.writeMicroseconds(PULSE_STOP);
    Serial.println(F("  3초 경과 - 정지"));
  }
}
