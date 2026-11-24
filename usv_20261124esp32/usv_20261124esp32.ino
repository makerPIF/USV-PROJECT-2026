/* =====================================================================
   무인정(USV) - 메인 제어 컨트롤러
   PUMPKIN IDEA FACTORY MAKERSPACE

   Board : DOIT ESP32 DevKit V1 (38핀) · ESP32-WROOM-32 · CP2102 · micro USB
           ※ 이 보드의 전원 입력 핀 이름은 VIN 이 아니라 "V5" 입니다.
             (왼쪽 열 맨 아래, USB 커넥터 옆. CMD 다음 칸)
           ※ SD0 SD1 SD2 SD3 CMD CLK 여섯 핀은 내장 플래시 전용입니다.
             비어 보여도 절대 쓰지 마세요 - 보드가 부팅하지 못합니다.
           ※ G34 G35 SP(36) SM(39) 은 입력 전용 - 출력으로 못 씁니다.
             이 펌웨어는 G34 를 배터리 전압 측정(입력)에만 씁니다.
   Sensor: MPU6050 (I2C) + u-blox NEO-8M (UART2)
   Motor : BLDC A2212 x2 + HobbyWing SeaKing V4 30A x2
           Running Mode = Forward and Reverse (정지 1500us, 2026-11 측정)
   Link  : Wi-Fi 공유기 -> WebSocket 텔레메트리 & 조종 (GCS 단독)
           NET_MODE 2 = 인터넷 공유기(주소 자동, usv.local)  <- 기본값
           NET_MODE 1 = 미니 공유기(192.168.8.20 고정)
           공유기를 못 찾으면 스스로 핫스팟(USV-DIRECT)이 됩니다.

   ※ 이 배에는 RC 조종기가 없습니다. 조종·영상·센서 모두 Wi-Fi 하나로
      오갑니다. 그래서 Wi-Fi가 끊기면 배를 멈출 방법이 "펌웨어가 스스로
      멈추는 것" 뿐입니다. 아래 네 가지가 그 역할을 합니다.
        1) 통신 두절 1.0초   -> 모터 중립 정지
        2) 하드웨어 워치독   -> 펌웨어가 멈추면 3초 뒤 보드 자동 재부팅
                               (재부팅 중에는 ESC 신호가 끊겨 모터도 정지)
        3) 홈 거리 제한      -> 설정 반경을 벗어나면 정지 + 시동 해제
        4) 저전압 / 전복     -> 정지 + 시동 해제
      이 중 하나라도 빼면 배를 세울 수단이 사라집니다. 고치지 마세요.

   ※ 이 펌웨어는 추진 방식과 무관합니다.
      수중 샤프트 구동이든 에어 프로펠러든, 좌우 모터를 차동으로 돌리는
      방식은 같으므로 코드를 고칠 필요가 없습니다.

   빌드 준비 (Arduino IDE)
   -----------------------------------------------------------------
   1) 보드 매니저 URL:
      https://espressif.github.io/arduino-esp32/package_esp32_index.json
      -> "esp32 by Espressif Systems" 설치
      -> 보드: "ESP32 Dev Module"  (DOIT ESP32 DEVKIT V1 을 골라도 됩니다)
      -> 포트가 안 보이면 CP210x VCP 드라이버(Silicon Labs)를 설치하세요
      ※ 스케치 경로에 한글이 들어 있으면 컴파일이 실패합니다.
        "grpc: error while marshaling: string field contains invalid UTF-8"
        이 뜨면 C:\usv\ 처럼 영문 경로로 옮겨서 컴파일하세요.
   2) 라이브러리 매니저에서 설치
        - ArduinoJson              (Benoit Blanchon)
        - Adafruit MPU6050         (+ Adafruit Unified Sensor, Adafruit BusIO)
        - TinyGPSPlus              (Mikal Hart)
        - ESP32Servo               (Kevin Harrington)
   3) 비동기 웹서버 2종 - 반드시 "짝"을 맞춰야 합니다
        라이브러리 매니저에서 ESP32Async 의 것 두 개를 설치하세요.
          - "ESP Async WebServer"   (ESP32Async)
          - "Async TCP"             (ESP32Async, 이름에 공백이 있습니다)

        ※ 옛날 이름인 "AsyncTCP"(공백 없음) 또는 "ESPAsyncWebServer"(공백 없음)가
          이미 깔려 있으면 반드시 삭제하세요. 둘이 섞이면 이 오류가 납니다.
            error: passing 'const AsyncServer' as 'this' argument
                   discards qualifiers [-fpermissive]
          새 웹서버는 status() 가 const 이기를 기대하는데, 옛 AsyncTCP 는
          const 가 아니어서 생기는 충돌입니다. 버전 문제가 아니라 "짝" 문제입니다.
   4) 도구 > Partition Scheme > "Default 4MB with spiffs"
      도구 > Flash Size > 4MB

   ※ ESC 의 정지 펄스는 반드시 측정해서 맞추세요.
      firmware/esc_find_neutral 을 올리고 ESC 에 배터리를 연결했을 때
      길게 "삐———" 가 울리는 값이 그 ESC 의 정지입니다.
      이 배의 SeaKing V4 는 1500us 였고, 그래서 ESC_FORWARD_ONLY 가 0 입니다.
      눈으로는 알 수 없습니다. 틀리면 "멈추라는 신호" 가 "전속" 이 됩니다.
   ===================================================================== */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <TinyGPSPlus.h>
#include <ESP32Servo.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

/* ===================== 1. 사용자 설정 ===================== */

/* ===== ESC 종류 =====  ★ 배를 물에 띄우기 전에 반드시 확인하세요
     1 = 전진 전용 ESC   : 정지 1000us.  후진·제자리회전 없음
     0 = 양방향(3D) ESC  : 정지 1500us.  후진·제자리회전 가능   ★ 현재 설정

   ※ 2026-11 측정 : 이 배에 달린 HobbyWing SeaKing V4 30A 는
     firmware/esc_find_neutral 로 확인한 결과 1500us 에서 정지했습니다.
     즉 Running Mode 가 "Forward and Reverse" 입니다. 그래서 0 입니다.

     1 로 두었을 때 무슨 일이 있었나 — 펌웨어가 정지 신호로 1000us 를
     내보냈는데, 양방향 ESC 는 그 값을 "풀 후진" 으로 읽습니다. 그래서
     GCS 에 "좌현 1000us / 우현 1000us" 가 찍히는데도 모터 두 개가
     계속 돌았습니다. 보드는 멀쩡했고 ESC 와 약속한 숫자가 달랐던 것입니다.

   ※ ESC 를 바꾸거나 프로그램 박스로 Running Mode 를 고쳤다면,
     반드시 esc_find_neutral 로 정지 펄스를 다시 재고 이 값을 맞추세요.
     눈으로는 알 수 없고, 틀리면 "멈추라는 신호" 가 "전속" 이 됩니다. */
#define ESC_FORWARD_ONLY  0

/* ===== 스로틀 0 에서 제자리 회전을 허용할 것인가 =====
     0 = 안 함. 스로틀을 가운데로 두면 조향이 얼마든 양쪽 다 정지  ★ 현재 설정
     1 = 함.   좌우 모터를 반대로 돌려 그 자리에서 뱃머리를 돌립니다

   양방향 ESC 라서 1 도 가능합니다. 좁은 곳에서 방향을 바꿀 때 아주 편합니다.
   다만 "스로틀이 가운데인데 모터가 돈다" 가 되므로, 학생이 처음 잡을 때는
   놀라기 쉽습니다. 익숙해진 뒤에 1 로 바꾸세요.                        */
#define ALLOW_SPIN_IN_PLACE  0

/* ===== 네트워크 =========================================================
   NET_MODE - 쓰는 공유기에 맞춰 이 숫자 하나만 바꿉니다.

     2 = 인터넷 공유기 (교실·실내 시험용)      ★ 현재 설정
         공유기가 주는 주소를 그대로 받습니다(DHCP). 그래서 ipTIME,
         KT, SK브로드밴드, LG U+ 어느 공유기에 붙여도 대역이 자동으로
         맞습니다. GCS 주소칸에는 아래 둘 중 아무거나 넣으면 됩니다.
             usv.local          <- 주소가 바뀌어도 늘 통합니다 (권장)
             시리얼에 찍힌 IP   <- 확실한 방법

     1 = 미니 공유기 (물가 운용용)
         GL.iNet 계열의 192.168.8.x 를 고정으로 씁니다. 늘 같은 주소라
         GCS 주소를 외워 쓸 수 있습니다.

   ※ 왜 모드가 둘인가 : 공유기마다 나눠 주는 IP 대역이 다릅니다.
      (ipTIME 192.168.0.x / KT 172.30.1.x / SKB 192.168.35.x /
       LG U+ 192.168.219.x / GL.iNet 192.168.8.x)
      여기에 고정 IP 를 못 박으면 대역이 다른 공유기에서는 "연결됨" 으로
      찍히면서도 실제로는 없는 주소가 되어 GCS 가 절대 못 붙습니다.
      모드 2 는 그 문제가 원천적으로 없습니다.

   ※ 물가에 나갈 때 1 로 되돌리고, 카메라 펌웨어도 같이 바꾸세요.
*/
#define NET_MODE  2

// --- 공유기 접속 정보 (NET_MODE 와 무관하게 여기만 고칩니다) ---
//     ※ ESP32 는 2.4GHz 만 씁니다. 5GHz 전용 SSID 에는 붙지 못합니다.
static const char* WIFI_SSID = "PIFmaker";
static const char* WIFI_PASS = "33138282";

/* --- mDNS 이름 -----------------------------------------------------------
   usv.local 로 접속할 수 있게 해 줍니다. 윈도우 10/11 과 맥은 따로
   설치할 것이 없습니다. IP 를 몰라도 되니 인터넷 공유기에서 특히 편합니다.
   카메라는 usvcam.local 입니다(카메라 펌웨어에 들어 있습니다). */
static const char* MDNS_NAME = "usv";

/* --- NET_MODE 1 에서만 쓰는 고정 IP ------------------------------------- */
IPAddress LOCAL_IP (192, 168, 8, 20);   // 이 보드(USV 본체)
IPAddress GATEWAY  (192, 168, 8, 1);    // 공유기
IPAddress SUBNET   (255, 255, 255, 0);
IPAddress DNS1     (192, 168, 8, 1);

/* --- 공유기를 못 찾으면 스스로 핫스팟이 되게 할까 ------------------------
   1 = 25초 안에 공유기에 못 붙으면 이 보드가 직접 Wi-Fi 를 만듭니다.
       노트북을 아래 SSID 로 붙이고 GCS 주소칸에 192.168.4.1 을 넣으면
       조종과 센서는 그대로 됩니다.
       ※ 이 모드에서는 카메라 영상이 나오지 않습니다. 카메라는 공유기에
         붙어 있어야 하기 때문입니다. 조종·센서만 쓰는 비상용입니다.
   0 = 끔 (공유기에 못 붙으면 그대로 멈춰 있습니다) */
#define ENABLE_AP_FALLBACK  1
static const char* AP_SSID = "USV-DIRECT";
static const char* AP_PASS = "pumpkin2026";   // 8자 이상이어야 합니다

// 배터리 (3S LiPo 기준)
static const float VBAT_DIV_RATIO = 5.545f;  // (100k + 22k) / 22k
//  전압 보정 계수는 Cfg.vbatCal 로 옮겨졌습니다 (GCS에서 조정).
static const float VBAT_WARN      = 10.8f;   // 3.60V/셀 : 경고
static const float VBAT_CRIT      = 10.2f;   // 3.40V/셀 : 즉시 복귀

// 조종 특성 — 기본값. 실제 값은 아래 Cfg 구조체에 들어가며 GCS에서 바꿉니다.
static const float DEF_STEER_GAIN = 0.65f;   // 회전 민감도 (0.2~1.0)
static const float DEF_THR_SLEW   = 2.0f;    // 초당 스로틀 변화 한계 (급가속 방지)
static const float DEF_DEADBAND   = 0.05f;   // 스틱/키 중립 불감대
static const float DEF_THR_LIMIT  = 0.85f;   // 최대 출력 제한 (모터 보호)

// 페일세이프
//  RC 조종기가 없으므로 통신 두절 판정을 1.5초 -> 1.0초로 줄였습니다.
//  (GCS는 조종을 안 해도 20Hz로 drive 명령을 보내므로 1.0초면 넉넉합니다)
static const uint32_t GCS_TIMEOUT_MS  = 1000; // GCS 명령 끊김 판정
static const uint32_t TELEM_PERIOD_MS = 100;  // 10 Hz 텔레메트리
static const uint8_t  WDT_TIMEOUT_S   = 3;    // 워치독: 3초간 루프가 멈추면 재부팅

// 홈 거리 제한 (지오펜스)
//  시동을 건 자리를 '홈'으로 잡고, 이 반경을 넘으면 스스로 정지합니다.
//  0 으로 두면 기능이 꺼집니다. 운용 전 반드시 현장에 맞게 정하세요.
static const float DEF_FENCE_RADIUS_M = 150.0f;

/* ===== 현장 설정 (GCS에서 바꾸고 보드에 저장됩니다) =====
   아두이노 IDE 없이 물가에서 바로 조정하기 위한 값들입니다.
   NVS(Preferences)에 저장되므로 전원을 껐다 켜도 유지됩니다.
   ※ 시동(ARM)이 걸린 상태에서는 변경을 거부합니다.            */
struct Cfg {
  float    fence;      // 홈 거리 제한 (m). 0 = 끔
  float    vbatCal;    // 전압 보정 계수
  float    steerGain;  // 회전 민감도
  float    thrLimit;   // 최대 출력
  float    thrSlew;    // 가속 제한
  float    deadband;   // 중립 불감대
  uint8_t  invL;       // 좌현 모터 방향 반전
  uint8_t  invR;       // 우현 모터 방향 반전
  uint8_t  swapLR;     // 좌우 바꿈
};
Cfg cfg;
Preferences prefs;

// 값마다 허용 범위를 둡니다. 범위를 벗어난 입력은 잘라서 받습니다.
static float clampRange(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void cfgDefaults() {
  cfg.fence     = DEF_FENCE_RADIUS_M;
  cfg.vbatCal   = 1.000f;
  cfg.steerGain = DEF_STEER_GAIN;
  cfg.thrLimit  = DEF_THR_LIMIT;
  cfg.thrSlew   = DEF_THR_SLEW;
  cfg.deadband  = DEF_DEADBAND;
  cfg.invL = 0; cfg.invR = 0; cfg.swapLR = 0;
}

void cfgSanitize() {
  // 지오펜스는 0(끔) 이거나 10~1000 m. 1 m 같은 값으로 배가 즉시 멈추는 것을 막습니다.
  if (cfg.fence > 0) cfg.fence = clampRange(cfg.fence, 10.0f, 1000.0f);
  if (cfg.fence < 0) cfg.fence = 0;
  cfg.vbatCal   = clampRange(cfg.vbatCal,   0.80f, 1.25f);
  cfg.steerGain = clampRange(cfg.steerGain, 0.20f, 1.00f);
  cfg.thrLimit  = clampRange(cfg.thrLimit,  0.20f, 1.00f);
  cfg.thrSlew   = clampRange(cfg.thrSlew,   0.50f, 10.0f);
  cfg.deadband  = clampRange(cfg.deadband,  0.00f, 0.20f);
  cfg.invL   = cfg.invL   ? 1 : 0;
  cfg.invR   = cfg.invR   ? 1 : 0;
  cfg.swapLR = cfg.swapLR ? 1 : 0;
}

void cfgLoad() {
  cfgDefaults();
  prefs.begin("usv", true);                      // 읽기 전용
  cfg.fence     = prefs.getFloat("fence",  cfg.fence);
  cfg.vbatCal   = prefs.getFloat("vcal",   cfg.vbatCal);
  cfg.steerGain = prefs.getFloat("sgain",  cfg.steerGain);
  cfg.thrLimit  = prefs.getFloat("tlim",   cfg.thrLimit);
  cfg.thrSlew   = prefs.getFloat("tslew",  cfg.thrSlew);
  cfg.deadband  = prefs.getFloat("dband",  cfg.deadband);
  cfg.invL      = prefs.getUChar("invL",   cfg.invL);
  cfg.invR      = prefs.getUChar("invR",   cfg.invR);
  cfg.swapLR    = prefs.getUChar("swap",   cfg.swapLR);
  prefs.end();
  cfgSanitize();
}

void cfgSave() {
  cfgSanitize();
  prefs.begin("usv", false);                     // 쓰기
  prefs.putFloat("fence", cfg.fence);
  prefs.putFloat("vcal",  cfg.vbatCal);
  prefs.putFloat("sgain", cfg.steerGain);
  prefs.putFloat("tlim",  cfg.thrLimit);
  prefs.putFloat("tslew", cfg.thrSlew);
  prefs.putFloat("dband", cfg.deadband);
  prefs.putUChar("invL",  cfg.invL);
  prefs.putUChar("invR",  cfg.invR);
  prefs.putUChar("swap",  cfg.swapLR);
  prefs.end();
}

/* ===================== 2. 핀 배치 ===================== */
#define PIN_ESC_L     25   // 좌현 ESC 신호
#define PIN_ESC_R     26   // 우현 ESC 신호
#define PIN_SDA       21   // MPU6050
#define PIN_SCL       22
#define PIN_GPS_RX    16   // ESP32 RX2  <- NEO-8M TX
#define PIN_GPS_TX    17   // ESP32 TX2  -> NEO-8M RX
// GPIO 4, 13 : 예전 RC 수신기(iBUS)용. 지금은 비어 있습니다
#define PIN_VBAT      34   // ADC1_CH6 (입력 전용 핀)
#define PIN_BUZZER    27
#define PIN_LED       2    // 온보드 LED

/* ===================== 3. ESC 펄스 정의 (3D 모드) ===================== */
#if ESC_FORWARD_ONLY
static const int PULSE_NEUTRAL = 1000;  // 전진 전용 : 정지 = 최저 펄스
#else
static const int PULSE_NEUTRAL = 1500;  // 양방향 : 정지 = 중립
#endif
static const int PULSE_FWD_MAX = 2000;  // 최대 전진
static const int PULSE_REV_MAX = 1000;  // 최대 후진
static const int PULSE_DEAD    = 30;    // 중립 부근 무출력 폭(us)

/* ===================== 4. 전역 객체 ===================== */
AsyncWebServer  server(80);
AsyncWebSocket  ws("/ws");
Adafruit_MPU6050 mpu;
TinyGPSPlus      gps;
HardwareSerial   GpsSerial(2);
Servo escL, escR;

/* ===================== 5. 상태 변수 ===================== */
struct Attitude { float roll = 0, pitch = 0, yaw = 0; };
Attitude att;

float ax = 0, ay = 0, az = 0;     // g
float gx = 0, gy = 0, gz = 0;     // deg/s
float tempC = 0;
float gyroBiasX = 0, gyroBiasY = 0, gyroBiasZ = 0;

double lat = 0, lon = 0;
float  gpsSpeed = 0, gpsCourse = 0, gpsAlt = 0, hdop = 99;
uint8_t sats = 0;
bool   gpsFix = false;

float  vbat = 0;
int    rssi = 0;

float  cmdThr = 0, cmdStr = 0;    // GCS 명령 (-1..1)
float  outThr = 0, outStr = 0;    // 슬루 적용된 실제 출력
int    pulseL = PULSE_NEUTRAL, pulseR = PULSE_NEUTRAL;

bool   armed = false;
bool   mpuOk = false;
const char* failsafeReason = "";

// 홈(시동을 건 자리)과 거기서의 거리
double homeLat = 0, homeLon = 0;
bool   homeSet = false;
float  homeDist = 0;

uint32_t seq = 0;
uint32_t lastGcsCmd = 0;
uint32_t tTelem = 0, tImu = 0, tSlow = 0;

/* ===================== 6. 유틸 ===================== */
static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float applyDeadband(float v) {
  const float db = cfg.deadband;
  if (db <= 0) return v;
  if (fabs(v) < db) return 0;
  return (v > 0) ? (v - db) / (1 - db)
                 : (v + db) / (1 - db);
}

void beep(uint16_t ms, uint8_t times = 1) {
  for (uint8_t i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(ms);
    digitalWrite(PIN_BUZZER, LOW);  if (i < times - 1) delay(ms);
  }
}

/* ===================== 7. ESC ===================== */
void escWrite(int l, int r) {
  pulseL = constrain(l, PULSE_REV_MAX, PULSE_FWD_MAX);
  pulseR = constrain(r, PULSE_REV_MAX, PULSE_FWD_MAX);
  escL.writeMicroseconds(pulseL);
  escR.writeMicroseconds(pulseR);
}
void escNeutral() { escWrite(PULSE_NEUTRAL, PULSE_NEUTRAL); }

// -1..1 값을 3D 모드 펄스로 변환 (중립 부근에는 데드밴드를 둠)
int mixToPulse(float v) {
#if ESC_FORWARD_ONLY
  v = clampf(v, 0.0f, 1.0f);              // 후진 없음 : 음수는 정지로
  if (v < 0.02f) return PULSE_NEUTRAL;
  return PULSE_NEUTRAL + PULSE_DEAD + (int)(v * (PULSE_FWD_MAX - PULSE_NEUTRAL - PULSE_DEAD));
#else
  v = clampf(v, -1.0f, 1.0f);
  if (fabs(v) < 0.02f) return PULSE_NEUTRAL;
  if (v > 0) return PULSE_NEUTRAL + PULSE_DEAD + (int)(v * (PULSE_FWD_MAX - PULSE_NEUTRAL - PULSE_DEAD));
  else       return PULSE_NEUTRAL - PULSE_DEAD + (int)(v * (PULSE_NEUTRAL - PULSE_REV_MAX - PULSE_DEAD));
#endif
}

/* ESC 초기화(아밍) : ESC 는 정지 신호를 일정 시간 받아야 깨어납니다.
   전진 전용이면 1000us, 양방향이면 1500us 가 자동으로 나갑니다. */
void escArmSequence() {
  escL.setPeriodHertz(50);
  escR.setPeriodHertz(50);
  escL.attach(PIN_ESC_L, PULSE_REV_MAX, PULSE_FWD_MAX);
  escR.attach(PIN_ESC_R, PULSE_REV_MAX, PULSE_FWD_MAX);
  escNeutral();
  Serial.println(F("[ESC] 중립 신호 3초 출력 (ESC 기동음 대기)..."));
  delay(3000);
}

/* ===================== 8. 센서 ===================== */
void imuCalibrate() {
  Serial.println(F("[IMU] 자이로 바이어스 보정 - 배를 움직이지 마세요 (2초)"));
  const int N = 400;
  double sx = 0, sy = 0, sz = 0;
  sensors_event_t a, g, t;
  for (int i = 0; i < N; i++) {
    mpu.getEvent(&a, &g, &t);
    sx += g.gyro.x; sy += g.gyro.y; sz += g.gyro.z;
    delay(5);
  }
  gyroBiasX = sx / N; gyroBiasY = sy / N; gyroBiasZ = sz / N;
  Serial.println(F("[IMU] 보정 완료"));
}

void imuUpdate(float dt) {
  if (!mpuOk) return;
  sensors_event_t a, g, t;
  mpu.getEvent(&a, &g, &t);

  ax = a.acceleration.x / 9.80665f;
  ay = a.acceleration.y / 9.80665f;
  az = a.acceleration.z / 9.80665f;
  gx = (g.gyro.x - gyroBiasX) * 57.2957795f;
  gy = (g.gyro.y - gyroBiasY) * 57.2957795f;
  gz = (g.gyro.z - gyroBiasZ) * 57.2957795f;
  tempC = t.temperature;

  // 가속도계 기반 절대 각도
  float accRoll  = atan2f(ay, sqrtf(ax * ax + az * az)) * 57.2957795f;
  float accPitch = atan2f(-ax, sqrtf(ay * ay + az * az)) * 57.2957795f;

  // 상보 필터 (파도 충격에 강하도록 자이로 비중 98%)
  const float K = 0.98f;
  att.roll  = K * (att.roll  + gx * dt) + (1 - K) * accRoll;
  att.pitch = K * (att.pitch + gy * dt) + (1 - K) * accPitch;

  // 요는 자이로 적분 + GPS 진행방위로 보정 (지자기 센서 미탑재)
  att.yaw += gz * dt;
  if (gpsFix && gpsSpeed > 0.6f) {           // 0.6 m/s 이상 이동 중일 때만 신뢰
    float err = gpsCourse - att.yaw;
    while (err >  180) err -= 360;
    while (err < -180) err += 360;
    att.yaw += 0.02f * err;
  }
  while (att.yaw >= 360) att.yaw -= 360;
  while (att.yaw <    0) att.yaw += 360;
}

void gpsUpdate() {
  while (GpsSerial.available()) gps.encode(GpsSerial.read());
  if (gps.location.isValid()) { lat = gps.location.lat(); lon = gps.location.lng(); }
  if (gps.speed.isValid())    gpsSpeed  = gps.speed.mps();
  if (gps.course.isValid())   gpsCourse = gps.course.deg();
  if (gps.altitude.isValid()) gpsAlt    = gps.altitude.meters();
  if (gps.hdop.isValid())     hdop      = gps.hdop.hdop();
  if (gps.satellites.isValid()) sats    = gps.satellites.value();
  gpsFix = gps.location.isValid() && sats >= 4 && gps.location.age() < 3000;
}

void vbatUpdate() {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(PIN_VBAT);
  float mv = acc / 16.0f;
  float v  = (mv / 1000.0f) * VBAT_DIV_RATIO * cfg.vbatCal;
  vbat = (vbat == 0) ? v : (vbat * 0.9f + v * 0.1f);   // 저역통과
}

/* ===================== 9. 홈 거리 (지오펜스) ===================== */
// 두 위경도 사이의 거리(m). 이 크기의 운용 범위에서는 이 근사로 충분합니다.
float distanceM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371000.0;
  double dLat = radians(lat2 - lat1);
  double dLon = radians(lon2 - lon1) * cos(radians((lat1 + lat2) / 2.0));
  return (float)(R * sqrt(dLat * dLat + dLon * dLon));
}

void homeUpdate() {
  if (homeSet && gpsFix) homeDist = distanceM(homeLat, homeLon, lat, lon);
}

// 시동을 걸 때 현재 위치를 홈으로 잡습니다(측위가 되어 있을 때만).
void homeCapture() {
  if (gpsFix) { homeLat = lat; homeLon = lon; homeSet = true; homeDist = 0; }
}

/* ===================== 10. 제어 루프 ===================== */
void controlUpdate(float dt) {
  float tThr = 0, tStr = 0;
  failsafeReason = "";

  // --- 목표값 산출 (조종은 GCS 하나뿐입니다) ---
  bool gcsAlive = (millis() - lastGcsCmd) < GCS_TIMEOUT_MS;

  if (gcsAlive) {
    tThr = cmdThr; tStr = cmdStr;
  } else {
    tThr = 0; tStr = 0;                  // 통신이 끊기면 무조건 중립
    if (armed) failsafeReason = "GCS_LINK_LOST";
  }

  // --- 안전 조건 ---
  if (!armed)                { tThr = 0; tStr = 0; }
  if (vbat > 5 && vbat < VBAT_CRIT) {           // 5V 미만은 센서 미연결로 간주
    tThr = 0; tStr = 0; armed = false; failsafeReason = "BATTERY_CRITICAL";
  }
  if (mpuOk && fabs(att.roll) > 70) {           // 전복 감지
    tThr = 0; tStr = 0; armed = false; failsafeReason = "CAPSIZED";
  }
  if (cfg.fence > 0 && homeSet && gpsFix && homeDist > cfg.fence) {
    tThr = 0; tStr = 0; armed = false; failsafeReason = "GEOFENCE";
  }

  /* --- 슬루 레이트 (급격한 출력 변화 억제) ---
     가속은 천천히, 감속은 빠르게 합니다. 올릴 때 완만한 것은 모터와
     배터리를 보호하기 위해서지만, "멈춰라" 는 늦으면 안 됩니다.
     그래서 0 에 가까워지는 방향으로는 3배 빠르게 움직입니다.       */
  bool  slowing = fabsf(tThr) < fabsf(outThr);
  float maxStep = cfg.thrSlew * dt * (slowing ? 3.0f : 1.0f);
  outThr += clampf(tThr - outThr, -maxStep, maxStep);
  outStr += clampf(tStr - outStr, -maxStep * 2, maxStep * 2);
  if (fabsf(outThr) < 0.005f) outThr = 0;      // 0 부근 잔류값 제거
  if (fabsf(outStr) < 0.005f) outStr = 0;

  // --- 차동 믹싱 ---
#if ESC_FORWARD_ONLY
  /* 전진 전용 : "곱셈" 믹싱을 씁니다.
       l = 스로틀 x (1 + 조향)
       r = 스로틀 x (1 - 조향)

     왜 덧셈이 아니라 곱셈인가
     -----------------------------------------------------------------
     예전에는 l = 스로틀 + 조향 이었습니다. 그러면 스로틀이 0 이어도
     조향만 주면 한쪽 모터가 돕니다. 패드를 가운데로 되돌려도 좌우로
     치우쳐 있으면 모터가 계속 도는 현상이 바로 이것이었습니다.

     곱셈으로 바꾸면 스로틀이 0 일 때 양쪽 모두 0 이 됩니다.
     "스로틀을 가운데로 = 정지" 가 어떤 조향 값에서도 성립합니다.
     배로서도 이쪽이 맞습니다. 물 위에서 추력이 없으면 어차피
     뱃머리가 돌지 않으므로, 조향은 속도가 있을 때만 의미가 있습니다. */
  float base = clampf(outThr, 0.0f, 1.0f);
  float s    = clampf(outStr * cfg.steerGain, -1.0f, 1.0f);

  float l, r;
  if (base < 0.02f) {            // ★ 스로틀 중립 = 무조건 양쪽 정지
    l = 0.0f; r = 0.0f;
  } else {
    l = base * (1.0f + s);
    r = base * (1.0f - s);
  }

  l = clampf(l, 0.0f, 1.0f);
  r = clampf(r, 0.0f, 1.0f);
  float peak = max(l, r);
  if (peak > cfg.thrLimit && peak > 0.001f) {
    float k = cfg.thrLimit / peak; l *= k; r *= k;
  }
#else
  /* 양방향(3D) : 덧셈 믹싱.
       전진/후진   : 양쪽 같은 방향
       제자리 회전 : 스로틀 0 에서 좌우 반대 방향 (음수 출력이 되므로 가능) */
  float l = outThr + outStr * cfg.steerGain;
  float r = outThr - outStr * cfg.steerGain;

#if !ALLOW_SPIN_IN_PLACE
  /* ★ 스로틀 중립 = 무조건 양쪽 정지.
     이것이 없으면 스로틀이 가운데인데도 조향만으로 좌우 모터가 반대로
     돌아갑니다(제자리 회전). 기능으로는 맞지만, 처음 잡는 학생에게는
     "가운데로 뒀는데 왜 도나" 가 됩니다. 그래서 기본은 꺼 둡니다.     */
  if (fabsf(outThr) < 0.02f) { l = 0.0f; r = 0.0f; }
#endif

  // 포화 시 비율 유지
  float peak = max(fabs(l), fabs(r));
  if (peak > 1.0f) { l /= peak; r /= peak; }

  l = clampf(l, -cfg.thrLimit, cfg.thrLimit);
  r = clampf(r, -cfg.thrLimit, cfg.thrLimit);
#endif

  // --- 모터 방향 보정 (선을 바꿔 끼우는 대신 GCS에서 설정) ---
  float oL = cfg.swapLR ? r : l;
  float oR = cfg.swapLR ? l : r;
#if !ESC_FORWARD_ONLY
  if (cfg.invL) oL = -oL;
  if (cfg.invR) oR = -oR;
#endif  // 전진 전용에서는 모터 3선 중 2개를 바꿔 끼워 회전 방향을 맞춥니다

  escWrite(mixToPulse(oL), mixToPulse(oR));
}

/* ===================== 11. 텔레메트리 ===================== */
void sendTelemetry() {
  if (ws.count() == 0) return;

  StaticJsonDocument<640> d;
  d["seq"]  = seq++;
  d["t"]    = millis();
  d["roll"] = roundf(att.roll  * 10) / 10.0f;
  d["pitch"]= roundf(att.pitch * 10) / 10.0f;
  d["yaw"]  = roundf(att.yaw   * 10) / 10.0f;
  d["ax"]   = roundf(ax * 100) / 100.0f;
  d["ay"]   = roundf(ay * 100) / 100.0f;
  d["az"]   = roundf(az * 100) / 100.0f;
  d["gz"]   = roundf(gz * 10) / 10.0f;
  d["tmp"]  = roundf(tempC * 10) / 10.0f;
  d["lat"]  = lat;
  d["lon"]  = lon;
  d["spd"]  = roundf(gpsSpeed * 100) / 100.0f;
  d["cog"]  = roundf(gpsCourse * 10) / 10.0f;
  d["alt"]  = roundf(gpsAlt * 10) / 10.0f;
  d["sat"]  = sats;
  d["hdop"] = roundf(hdop * 100) / 100.0f;
  d["fix"]  = gpsFix ? 1 : 0;
  d["vbat"] = roundf(vbat * 100) / 100.0f;
  d["rssi"] = rssi;
  d["arm"]  = armed ? 1 : 0;
  d["home"] = homeSet ? roundf(homeDist * 10) / 10.0f : -1.0f;  // -1 = 홈 미설정
  d["fence"]= cfg.fence;
  d["thr"]  = roundf(outThr * 100) / 100.0f;
  d["str"]  = roundf(outStr * 100) / 100.0f;
  d["pl"]   = pulseL;
  d["pr"]   = pulseR;
  d["fs"]   = failsafeReason;

  char buf[700];
  size_t n = serializeJson(d, buf, sizeof(buf));
  ws.textAll(buf, n);
}

/* ===================== 12. 설정 송신 ===================== */
// client 가 nullptr 이면 접속한 모든 GCS 에게 보냅니다.
void sendCfg(AsyncWebSocketClient* client) {
  StaticJsonDocument<320> d;
  d["c"]         = "cfg";
  d["fence"]     = cfg.fence;
  d["vbatCal"]   = cfg.vbatCal;
  d["steerGain"] = cfg.steerGain;
  d["thrLimit"]  = cfg.thrLimit;
  d["thrSlew"]   = cfg.thrSlew;
  d["deadband"]  = cfg.deadband;
  d["invL"]      = cfg.invL;
  d["invR"]      = cfg.invR;
  d["swapLR"]    = cfg.swapLR;
  char buf[360];
  size_t n = serializeJson(d, buf, sizeof(buf));
  if (client) client->text(buf, n); else ws.textAll(buf, n);
}

void sendCfgErr(AsyncWebSocketClient* client, const char* msg) {
  StaticJsonDocument<160> d;
  d["c"]   = "cfgerr";
  d["msg"] = msg;
  char buf[200];
  size_t n = serializeJson(d, buf, sizeof(buf));
  if (client) client->text(buf, n); else ws.textAll(buf, n);
}

/* ===================== 12. WebSocket 수신 ===================== */
void handleCommand(AsyncWebSocketClient* client, const char* payload, size_t len) {
  // cfgset 은 항목이 9개라 256 바이트로는 모자랍니다.
  StaticJsonDocument<512> d;
  if (deserializeJson(d, payload, len)) return;
  const char* c = d["c"] | "";

  if (!strcmp(c, "drive")) {
#if ESC_FORWARD_ONLY
    cmdThr = applyDeadband(clampf(d["thr"] | 0.0f, 0.f, 1.f));   // 후진 입력 무시
#else
    cmdThr = applyDeadband(clampf(d["thr"] | 0.0f, -1.f, 1.f));
#endif
    cmdStr = applyDeadband(clampf(d["str"] | 0.0f, -1.f, 1.f));
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "arm")) {
    bool want = (d["v"] | 0) != 0;
    armed = want;
    cmdThr = cmdStr = 0; outThr = outStr = 0;
    if (armed) homeCapture();            // 시동을 건 자리를 홈으로
    beep(80, armed ? 2 : 1);
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "sethome")) {
    homeCapture();
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "stop")) {
    armed = false; cmdThr = cmdStr = 0; outThr = outStr = 0;
    escNeutral(); beep(300, 1);
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "ping")) {
    // 왕복 지연 측정용 즉시 에코
    char out[64];
    snprintf(out, sizeof(out), "{\"c\":\"pong\",\"id\":%ld}", (long)(d["id"] | 0));
    client->text(out);
    lastGcsCmd = millis();
  }
  else if (!strcmp(c, "beep")) {
    beep(120, 3);
  }
  // ---- 현장 설정 ----
  else if (!strcmp(c, "cfgget")) {
    sendCfg(client);
  }
  else if (!strcmp(c, "cfgset")) {
    // 안전: 시동이 걸린 상태에서는 설정을 바꾸지 않습니다.
    if (armed) { sendCfgErr(client, "시동을 끄고 변경하세요"); return; }

    JsonObject o = d["v"];
    if (o.isNull()) { sendCfgErr(client, "값이 없습니다"); return; }
    if (o.containsKey("fence"))     cfg.fence     = o["fence"];
    if (o.containsKey("vbatCal"))   cfg.vbatCal   = o["vbatCal"];
    if (o.containsKey("steerGain")) cfg.steerGain = o["steerGain"];
    if (o.containsKey("thrLimit"))  cfg.thrLimit  = o["thrLimit"];
    if (o.containsKey("thrSlew"))   cfg.thrSlew   = o["thrSlew"];
    if (o.containsKey("deadband"))  cfg.deadband  = o["deadband"];
    if (o.containsKey("invL"))      cfg.invL      = (uint8_t)(int)o["invL"];
    if (o.containsKey("invR"))      cfg.invR      = (uint8_t)(int)o["invR"];
    if (o.containsKey("swapLR"))    cfg.swapLR    = (uint8_t)(int)o["swapLR"];

    cfgSave();            // 범위 보정 후 NVS 저장
    beep(60, 2);
    sendCfg(nullptr);     // 접속한 모든 GCS에 갱신된 값을 알립니다
    Serial.printf("[CFG] 저장됨  fence=%.0fm vcal=%.3f sgain=%.2f tlim=%.2f\n",
                  cfg.fence, cfg.vbatCal, cfg.steerGain, cfg.thrLimit);
  }
  else if (!strcmp(c, "cfgreset")) {
    if (armed) { sendCfgErr(client, "시동을 끄고 변경하세요"); return; }
    cfgDefaults();
    cfgSave();
    beep(60, 3);
    sendCfg(nullptr);
    Serial.println(F("[CFG] 기본값으로 되돌렸습니다"));
  }
}

void onWsEvent(AsyncWebSocket* s, AsyncWebSocketClient* client,
               AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WS] 접속 #%u %s\n", client->id(), client->remoteIP().toString().c_str());
      lastGcsCmd = millis();
      sendCfg(client);          // 접속하면 현재 설정을 먼저 보냅니다
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] 해제 #%u\n", client->id());
      cmdThr = cmdStr = 0;
      break;
    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        data[len] = 0;
        handleCommand(client, (const char*)data, len);
      }
      break;
    }
    default: break;
  }
}

/* ===================== 13. setup / loop ===================== */
/* ===================== 네트워크 접속 =====================
   공유기 종류와 상관없이 "지금 어떤 주소로 떴는지" 를 분명히 알려 주는
   것이 이 함수의 목적입니다. GCS 가 붙지 않는 사고의 거의 전부가
   "주소를 잘못 알고 있었다" 에서 나옵니다.                              */

bool g_apMode = false;        // 공유기를 못 찾아 핫스팟으로 떴는가

static void netBanner() {
  IPAddress ip = WiFi.localIP();
  IPAddress gw = WiFi.gatewayIP();
  IPAddress sn = WiFi.subnetMask();

  Serial.println();
  Serial.println(F("  ====================================================="));
  Serial.println(F("   GCS 주소칸에 넣을 값"));
  Serial.printf ("     %s\n", ip.toString().c_str());
  Serial.printf ("     %s.local   (주소가 바뀌어도 통합니다)\n", MDNS_NAME);
  Serial.println(F("  ====================================================="));
  Serial.printf ("   게이트웨이 %s   서브넷 %s   RSSI %d dBm\n",
                 gw.toString().c_str(), sn.toString().c_str(), WiFi.RSSI());

  // --- 대역이 맞는지 직접 검사합니다 ---
  //  고정 IP 는 공유기 대역이 달라도 그대로 찍히기 때문에, 시리얼의 IP 만
  //  보고는 맞는지 알 수 없습니다. 그래서 여기서 계산해 알려 줍니다.
  uint32_t m = (uint32_t)sn;
  if (m && (((uint32_t)ip & m) != ((uint32_t)gw & m))) {
    Serial.println();
    Serial.println(F("   ★★ 경고 : 내 IP 와 공유기가 서로 다른 대역입니다."));
    Serial.println(F("      고정 IP 가 이 공유기에 맞지 않습니다."));
    Serial.println(F("      이대로는 GCS 가 절대 접속되지 않습니다."));
    Serial.println(F("      -> NET_MODE 를 2 로 바꿔 다시 올리세요."));
    beep(250, 5);
  }
  Serial.println();
}

void netConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                       // 지연 최소화 (중요)
  WiFi.setAutoReconnect(true);                // 끊기면 스스로 다시 붙습니다
  WiFi.setTxPower(WIFI_POWER_19_5dBm);        // 최대 송신 출력
  WiFi.setHostname(MDNS_NAME);                // 공유기 목록에 usv 로 보입니다

#if NET_MODE == 1
  WiFi.config(LOCAL_IP, GATEWAY, SUBNET, DNS1);
  Serial.println(F("[WiFi] NET_MODE 1 - 미니 공유기 / 고정 IP"));
#else
  Serial.println(F("[WiFi] NET_MODE 2 - 인터넷 공유기 / 주소 자동(DHCP)"));
#endif

  Serial.printf("[WiFi] \"%s\" 에 접속 중", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) {
    delay(300); Serial.print('.');
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    netBanner();
    if (MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", 80);
      MDNS.addService("ws",   "tcp", 80);
    } else {
      Serial.println(F("[mDNS] 이름 등록 실패 - IP 로 접속하세요"));
    }
    digitalWrite(PIN_LED, HIGH);
    beep(80, 2);
    return;
  }

  // ----- 공유기에 붙지 못했습니다 -----
  Serial.println(F("[WiFi] 공유기에 붙지 못했습니다. 확인할 것:"));
  Serial.println(F("       · SSID 와 비밀번호가 맞는지 (대소문자 구분)"));
  Serial.println(F("       · 그 SSID 가 2.4GHz 인지 - ESP32 는 5GHz 를 못 씁니다"));
  Serial.println(F("       · 공유기와 너무 멀지 않은지"));

#if ENABLE_AP_FALLBACK
  Serial.println();
  Serial.println(F("[AP] 비상 핫스팟 모드로 전환합니다."));
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  g_apMode = true;
  Serial.println(F("  ====================================================="));
  Serial.printf ("   노트북을 이 Wi-Fi 에 붙이세요 :  %s\n", AP_SSID);
  Serial.printf ("   비밀번호 :  %s\n", AP_PASS);
  Serial.printf ("   GCS 주소칸 :  %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println(F("  ====================================================="));
  Serial.println(F("   ※ 이 모드에서는 카메라 영상이 나오지 않습니다."));
  Serial.println(F("     조종과 센서만 쓰는 비상용입니다."));
  Serial.println();
  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  digitalWrite(PIN_LED, HIGH);
  beep(120, 4);
#else
  Serial.println(F("[WiFi] 조종 수단이 없습니다. 모터는 돌지 않습니다."));
  beep(400, 3);
#endif
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n===== USV 컨트롤러 ====="));

  cfgLoad();                  // 저장된 현장 설정을 먼저 읽습니다
  Serial.printf("[CFG] fence=%.0fm vcal=%.3f sgain=%.2f tlim=%.2f slew=%.1f db=%.2f inv=%d%d swap=%d\n",
                cfg.fence, cfg.vbatCal, cfg.steerGain, cfg.thrLimit,
                cfg.thrSlew, cfg.deadband, cfg.invL, cfg.invR, cfg.swapLR);

  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VBAT, ADC_11db);

  // --- ESC 먼저 (부팅 순간 모터가 돌지 않도록) ---
  escArmSequence();

  // --- IMU ---
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (mpu.begin(0x68, &Wire)) {
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_500_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);   // 파도/모터 진동 억제
    mpuOk = true;
    imuCalibrate();
  } else {
    Serial.println(F("[IMU] MPU6050 미검출! SDA/SCL 배선을 확인하세요."));
  }

  // --- GPS ---
  GpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // --- Wi-Fi ---
  netConnect();

  // --- 웹서버 / WebSocket ---
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest* r) {
    char b[160];
    snprintf(b, sizeof(b), "{\"ok\":1,\"rssi\":%d,\"up\":%lu,\"heap\":%u}",
             WiFi.RSSI(), millis() / 1000, ESP.getFreeHeap());
    r->send(200, "application/json", b);
  });

  server.onNotFound([](AsyncWebServerRequest* r) {
    r->send(200, "text/html; charset=utf-8",
            "<meta charset='utf-8'><h2>USV Controller</h2>"
            "<p>GCS(index.html)를 PC에서 열고 이 보드의 IP로 접속하세요.</p>"
            "<p>WebSocket: <code>/ws</code></p>");
  });
  server.begin();

  // --- 워치독 (RC가 없는 구조에서 마지막 안전장치) ---
  //  루프가 WDT_TIMEOUT_S 동안 돌지 못하면 보드를 재부팅합니다.
  //  재부팅 중에는 ESC 신호가 끊기고, BLHeli_S ESC는 신호가 없으면
  //  스스로 모터를 세우므로 결과적으로 배가 멈춥니다.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = (uint32_t)WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  //  코어 3.x 는 부팅 때 이미 워치독을 켜 둡니다. 그 상태에서 init 을 다시
  //  부르면 ESP_ERR_INVALID_STATE 를 돌려주고 ("TWDT already initialized")
  //  우리가 정한 시간·재부팅 설정이 적용되지 않습니다.
  //  그때는 reconfigure 로 설정만 바꿔치웁니다.
  if (esp_task_wdt_init(&wdtCfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdtCfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);

  Serial.println(F("[SYS] 준비 완료. 아밍 전까지 모터는 정지 상태입니다."));
  Serial.printf("[SYS] ESC 모드: %s (정지 %dus)\n",
                ESC_FORWARD_ONLY ? "전진 전용" : "양방향(3D)", PULSE_NEUTRAL);
  Serial.printf("[SYS] 워치독 %ds, 홈 거리 제한 %.0fm (0=끔)\n",
                WDT_TIMEOUT_S, cfg.fence);
  tImu = millis();
}

void loop() {
  uint32_t now = millis();

  esp_task_wdt_reset();        // 워치독에게 "살아 있다"고 알림
  gpsUpdate();
  homeUpdate();

  // 200 Hz IMU + 제어
  if (now - tImu >= 5) {
    float dt = (now - tImu) / 1000.0f;
    tImu = now;
    imuUpdate(dt);
    controlUpdate(dt);
  }

  // 4 Hz 저속 항목
  if (now - tSlow >= 250) {
    tSlow = now;
    vbatUpdate();
    // 핫스팟 모드에서는 RSSI 가 없습니다. 0 으로 보내 GCS 가 "AP" 로 읽게 합니다.
    bool netUp = g_apMode ? true : WiFi.isConnected();
    rssi = g_apMode ? 0 : (WiFi.isConnected() ? WiFi.RSSI() : -127);
    // 상태 LED : 아밍=점등, 대기=느린 깜빡임, 링크단절=빠른 깜빡임
    if (armed)       digitalWrite(PIN_LED, HIGH);
    else if (netUp)  digitalWrite(PIN_LED, (now / 800) % 2);
    else             digitalWrite(PIN_LED, (now / 150) % 2);
    // 저전압 경고음
    if (vbat > 5 && vbat < VBAT_WARN) { static uint32_t tb = 0;
      if (now - tb > 4000) { tb = now; beep(60, 2); } }
  }

  // 10 Hz 텔레메트리
  if (now - tTelem >= TELEM_PERIOD_MS) {
    tTelem = now;
    sendTelemetry();
    ws.cleanupClients();
  }
}
