// ============================================================
// ESP32-C3 AI Bot '로미' 전체 코드 (Single-Core & Deep Sleep 최적화)
// - WiFi 하드웨어 프리징/WPA3 충돌 방지 패치(8.5dBm) 적용 완료
// - 타임아웃 기반 딥슬립 및 오디오/OLED/버튼 제어 완전 통합
// ============================================================

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Audio.h>
#include <FluxGarage_RoboEyes.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <time.h>

// ── 하드웨어 핀맵 설정 ──────────────────
#define I2S_BCLK 2
#define I2S_LRC 3
#define I2S_DOUT 1
#define AMP_SD 10
#define OLED_SDA 8
#define OLED_SCL 9
#define PIN_BUTTON 4

#define SCREEN_W 128
#define SCREEN_H 64

// ── 객체 생성 ──────────────────────────
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(display);
Audio audio;

// ── 네트워크 설정 ───────────────────────
// ⚠️ TODO: 아래에 본인의 Wi-Fi SSID와 비밀번호를 입력하세요.
const char *ssid = "YOUR_WIFI_SSID";
const char *password = "YOUR_WIFI_PASSWORD";
// ⚠️ TODO: 아래에 본인의 n8n 웹훅 URL을 입력하세요.
const String webhook_url = "https://YOUR_N8N_DOMAIN/webhook/YOUR_WEBHOOK_ID";

// ── 기기 상태 정의 ──────────────────────
enum BotState {
  STATE_WIFI_CONNECTING,
  STATE_WAITING_ALARM,
  STATE_THINKING,
  STATE_PLAYING_AUDIO,
  STATE_AWAKE_IDLE
};
BotState currentState = STATE_WIFI_CONNECTING;

// ── 전역 변수 (타이머 & 제어 플래그) ───────
const long ACTIVE_TIME_MS = 600000; // 10분 활성 대기
unsigned long awakeStartTime = 0;
unsigned long lastDisplayUpdate = 0;

volatile bool isAudioFinished = false;
bool btnPrevDown = false;

// 현재 화면에 표시된 마지막 상태 메세지
String lastStatusMessage = "";

// 마지막으로 조회한 시간 문자열 캐시 (매 프레임 깜빡임 없이 그리기 위함)
char timeString[6] = "--:--";
unsigned long audioStartTime = 0; // 스트리밍 안전장치 타이머

// ── 함수 원형 ─────────────────────────
void setupWiFi();
void syncTime();
void triggerWebhook();
void calculateAndEnterDeepSleep();
long getSecondsToNextWakeup(struct tm &timeinfo);
void checkAlarmTime();

// 주기적으로 화면 전체 갱신 (디스플레이 번쩍임 방지를 대비해 텍스트만 바뀔 때)
void updateDisplay(const char *status);

// ── 오디오 콜백 (전역 스코프) ────────────
void audio_info(const char *info) {
  Serial.printf("[AUDIO-INFO] [%lu ms] %s\n", millis(), info);
}
void audio_eof_mp3(const char *info) {
  unsigned long duration = millis() - audioStartTime;
  Serial.printf("[AUDIO-EOF] [%lu ms] MP3 File Finished. Actual Play Duration: "
                "%lu ms. Info: %s\n",
                millis(), duration, info);
  isAudioFinished = true;
}
void audio_eof_stream(const char *info) {
  unsigned long duration = millis() - audioStartTime;
  Serial.printf("[AUDIO-EOF] [%lu ms] Stream Finished. Actual Play Duration: "
                "%lu ms. Info: %s\n",
                millis(), duration, info);
  isAudioFinished = true;
}
void audio_eof_speech(const char *info) {
  unsigned long duration = millis() - audioStartTime;
  Serial.printf("[AUDIO-EOF] [%lu ms] Speech Finished. Actual Play Duration: "
                "%lu ms. Info: %s\n",
                millis(), duration, info);
  isAudioFinished = true;
}
void audio_showstation(const char *info) {
  Serial.printf("[AUDIO-STA] %s\n", info);
}
void audio_showstreamtitle(const char *info) {
  Serial.printf("[AUDIO-TITLE] %s\n", info);
}
void audio_bitrate(const char *info) {
  Serial.printf("[AUDIO-BITRATE] %s\n", info);
}
void audio_commercial(const char *info) {
  Serial.printf("[AUDIO-COMM] %s\n", info);
}
void audio_icyurl(const char *info) { Serial.printf("[AUDIO-URL] %s\n", info); }
void audio_lasthost(const char *info) {
  Serial.printf("[AUDIO-HOST] %s\n", info);
}

// ═══════════════════════════════════════
// SETUP
// ═══════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(1000);

  // 버튼 & 앰프 초기 식별
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, LOW); // 딥슬립 직후 앰프 끄기 (배터리 보존)

  // OLED & 눈동자 초기화
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000); // 1. I2C 통신 속도 극대화 (Fast Mode 400kHz 대역폭
                         // 개방하여 화면 딜레이 삭감)

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 FAILED");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 30);
    display.print("Booting WiFi...");
    display.display();
  }

  eyes.begin(SCREEN_W, SCREEN_H, 100);
  eyes.setPosition(DEFAULT);
  eyes.setMood(DEFAULT);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);

  // 오디오 초기화
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.forceMono(true); // 채널 병합
  audio.setVolume(21);   // MAX 볼륨 보장
  audio.setConnectionTimeout(5000, 15000);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
  }

  setupWiFi();
  syncTime();

  // 기상 원인 판별
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  awakeStartTime = millis();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO ||
      digitalRead(PIN_BUTTON) == LOW) {
    Serial.println(">>> Woke up by BUTTON");
    triggerWebhook();
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println(">>> Woke up by TIMER. Waiting for strict alarm time...");
    currentState = STATE_WAITING_ALARM;
    eyes.setMood(DEFAULT);
    eyes.setCuriosity(true); // 알람 대기중 표정
  } else {
    Serial.println(">>> Normal Boot");
    currentState = STATE_AWAKE_IDLE;
  }
}

// ═══════════════════════════════════════
// WiFi 연결 (하드웨어 결함 완전 패치 적용)
// ═══════════════════════════════════════
void setupWiFi() {
  Serial.print("Connecting WiFi");

  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK); // WPA3 거부 충돌 방어
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // 슈퍼미니 C3 보드의 전원부 하드웨어 결함(status=6 타임아웃/프리징)을 원천
  // 차단하기 위해 출력을 8.5dBm으로 고정
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_ps(WIFI_PS_NONE); // 최대 응답성 유지 (드롭 방지)

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 20000) {
    delay(500);
    Serial.printf(" [St:%d]", WiFi.status());
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(">>> WiFi Connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(">>> WiFi Failed! Restarting...");
    delay(2000);
    ESP.restart(); // 20초 타임아웃 무조건 재부팅
  }
}

// ═══════════════════════════════════════
// 시간 동기화 (응답 지연 블로킹 방어 10초 컷)
// ═══════════════════════════════════════
void syncTime() {
  configTime(9 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Syncing NTP Time...");
  int retries = 0;
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 10) && retries < 10) {
    delay(1000);
    Serial.print(".");
    retries++;
  }
  if (retries >= 10) {
    Serial.println(" Failed! (No block)");
  } else {
    Serial.println(" OK!");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
  }
}

// ═══════════════════════════════════════
// 웹훅 트리거 및 앰프 점화
// ═══════════════════════════════════════
void triggerWebhook() {
  currentState = STATE_THINKING;

  // "Downloading.." 텍스트 대신 호기심/집중 표정으로 즉시 고정 (UX 몰입감 상승)
  eyes.setMood(DEFAULT);
  eyes.setCuriosity(true);
  eyes.update();
  // http.GET() 다운로드가 블로킹되는 동안 눈동자가 이 표정 그대로 얼어붙게 되어
  // 진짜 생각하는 듯한 연출이 완성됩니다.

  digitalWrite(AMP_SD, LOW); // 앰프 끄기 (다운로드 전력집중)
  isAudioFinished = false;

  Serial.println("Downloading MP3 from n8n webhook to LittleFS...");

  if (WiFi.status() == WL_CONNECTED) {
    bool downloadSuccess = false;
    int retryCount = 0;

    while (!downloadSuccess && retryCount < 3) {
      if (retryCount > 0) {
        Serial.printf("Retrying download... (%d/3)\n", retryCount + 1);
        delay(1500); // 잠시 대기 후 재시도
      }

      HTTPClient http;
      WiFiClient client;
      WiFiClientSecure clientSecure;

      // HTTPS일 경우 인증서 검사를 우회하여 연결 안정성 보장 (connection
      // refused, 메모리 오류 방지)
      if (webhook_url.startsWith("https")) {
        clientSecure.setInsecure();
        http.begin(clientSecure, webhook_url);
      } else {
        http.begin(client, webhook_url);
      }

      http.setTimeout(45000); // 45초 대기 (무거운 TTS 생성 시 n8n 지연 대비)

      unsigned long reqStart = millis();
      int httpCode = http.GET();
      unsigned long ttfb =
          millis() - reqStart; // Time To First Byte (n8n AI 처리 대기 시간)

      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        File file = LittleFS.open("/tts.mp3", FILE_WRITE);
        if (file) {
          unsigned long dlStart = millis();
          int bytesWritten = http.writeToStream(&file);
          file.close();
          unsigned long dlTime =
              millis() - dlStart; // 실제 다운로드 및 저장 소요 시간

          Serial.printf("Download complete. [n8n Wait Time: %lu ms] [Download "
                        "Time: %lu ms] [Size: %d bytes]\n",
                        ttfb, dlTime, bytesWritten);
          downloadSuccess = true;
        } else {
          Serial.println("Failed to open file for writing!");
        }
      } else {
        Serial.printf("HTTP GET failed, error: %s (Code: %d)\n",
                      http.errorToString(httpCode).c_str(), httpCode);
      }
      http.end();
      retryCount++;
    }

    if (downloadSuccess) {
      // --- 🔍 디버깅: 다운로드된 파일 상태 확인 ---
      File debugFile = LittleFS.open("/tts.mp3", FILE_READ);
      if (debugFile) {
        Serial.printf("[DEBUG] LittleFS File Size: %u bytes\n",
                      debugFile.size());
        uint8_t head[4];
        if (debugFile.read(head, 4) == 4) {
          Serial.printf("[DEBUG] File Header HEX: %02X %02X %02X %02X\n",
                        head[0], head[1], head[2], head[3]);
          if (head[0] != 0xFF && head[0] != 0x49) {
            Serial.println("[DEBUG-WARNING] Invalid MP3 Header! Webhook "
                           "returned garbage/error message instead of audio!");
          }
        }
        debugFile.close();
      }
      // ------------------------------------

      // 재생 준비 완료
      currentState = STATE_PLAYING_AUDIO;
      eyes.setMood(HAPPY);
      digitalWrite(AMP_SD, HIGH); // 앰프 켜기
      delay(100);                 // 앰프가 완벽히 깨어나도록 대기시간

      audioStartTime = millis();
      audio.setVolume(21); // 재생 직전 볼륨 최대화 보장
      Serial.printf(">>> Starting local playback. Heap: %u bytes\n",
                    ESP.getFreeHeap());

      // ✅ [추가] 비행기 모드 시작: MP3 재생 중 WiFi 간섭(노이즈, CPU 인터럽트) 원천 차단
      WiFi.mode(WIFI_OFF);

      audio.connecttoFS(LittleFS, "/tts.mp3");

      // [Optimization] 재생 직후 버퍼가 충분히 채워질 때까지 미세한 여유를 주어 지직거림 방어
      for (int i = 0; i < 3; i++) {
        audio.loop();
        delay(1);
      } 
    } else {
      Serial.println("Download completely failed after 3 retries.");
      currentState = STATE_AWAKE_IDLE;
      eyes.setMood(DEFAULT);
    }
  } else {
    Serial.println("WiFi Disconnected!");
    currentState = STATE_AWAKE_IDLE;
    eyes.setMood(DEFAULT);
  }
}

// ═══════════════════════════════════════
// 타겟 알람 (07:30 / 19:00 정시 체크)
// ═══════════════════════════════════════
void checkAlarmTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10))
    return;

  int ch = timeinfo.tm_hour;
  int cm = timeinfo.tm_min;
  int cs = timeinfo.tm_sec; // 초 단위 추가!

  // 분(minute)뿐만 아니라 초(sec)가 딱 '0초'일 때만 실행!
  if ((ch == 7 && cm == 30 && cs == 0) || (ch == 19 && cm == 0 && cs == 0)) {
    Serial.println("$$$ Alarm time reached! $$$");
    triggerWebhook();
  }
}

// ═══════════════════════════════════════
// 딥슬립 타겟 시간 계산 로직
// ═══════════════════════════════════════
long getSecondsToNextWakeup(struct tm &timeinfo) {
  long currentTotalSecs =
      timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;

  // 알람 시간(07:30 / 19:00)의 '5분 전'에 미리 기상해 WiFi 준비
  long target1 = 7 * 3600 + 25 * 60;  // 07:25 (26700 sec)
  long target2 = 18 * 3600 + 55 * 60; // 18:55 (68100 sec)

  long diff1 = target1 - currentTotalSecs;
  if (diff1 <= 0)
    diff1 += 24 * 3600;

  long diff2 = target2 - currentTotalSecs;
  if (diff2 <= 0)
    diff2 += 24 * 3600;

  return min(diff1, diff2);
}

void calculateAndEnterDeepSleep() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    Serial.println("Time missing. Restarting to re-sync.");
    delay(1000);
    ESP.restart(); // 시간 오류 발생 시 깔끔하게 재부팅 (자기 치유)
  }

  long sleepSecs = getSecondsToNextWakeup(timeinfo);
  Serial.printf(">>> Entering Deep Sleep for %ld seconds...\n", sleepSecs);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 30);
  display.print("Entering Sleep Zz..");
  display.display();
  delay(1000);

  display.clearDisplay();
  display.display();

  // ESP32-C3 전용 GPIO 웨이크업(저전력 끄기 방지) 및 타이머 웨이크업
  esp_deep_sleep_enable_gpio_wakeup((1ULL << PIN_BUTTON),
                                    ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_sleep_enable_timer_wakeup(sleepSecs * 1000000ULL);
  esp_deep_sleep_start();
}

// ═══════════════════════════════════════
// 메인 루프 (1 Core & Non-blocking)
// ═══════════════════════════════════════
void loop() {
  unsigned long currentMillis = millis();

  // [1] 버튼 폴링 (디바운스 처리를 가볍게)
  bool btnDown = (digitalRead(PIN_BUTTON) == LOW);
  if (btnDown && !btnPrevDown) {
    Serial.println(">>> Button Pressed! <<<");

    // 로봇이 작업 중이거나 렉에 걸려 무한 루프에 빠져있더라도 즉시 이전 작업
    // 강제 중단 (인터럽트)
    if (currentState == STATE_PLAYING_AUDIO) {
      Serial.println("[INTERRUPT] Force stopping current audio...");
      digitalWrite(AMP_SD, LOW);
      audio.stopSong();
      isAudioFinished = false;
    }

    awakeStartTime = currentMillis; // 10분 타이머 완전 연장
    triggerWebhook();
  }
  btnPrevDown = btnDown;

  // [2] 상태 머신 제어
  if (currentState == STATE_PLAYING_AUDIO) {
    audio.loop(); // MP3 재생 루프 (블로킹 없음)

    // 오디오 콜백(EOF)에서 정상이탈 신호가 도달했는지 확인
    bool streamEnded = isAudioFinished;

    // 스트림이 완전히 종료되었거나, 라이브러리가 Stream lost로 멈췄을 때
    if (!audio.isRunning()) {
      // 웹훅 시작 후 3초(3000ms) 이상 지났다면, 정상 재생 후 끝났거나 끊긴
      // 것으로 간주하여 즉시 종료
      if (currentMillis - audioStartTime > 3000) {
        Serial.println("[AUDIO] Stream ended or lost. Forcing exit "
                       "(Anti-Infinite Loop)...");
        streamEnded = true;
      }
    }

    // 10초가 지났는데도 아예 시작조차 못 했을 경우 (연결 실패 방어)
    if (currentMillis - audioStartTime > 10000 && streamEnded == false &&
        !audio.isRunning()) {
      Serial.println("[AUDIO] Start failed! Forcing exit...");
      streamEnded = true;
    }

    if (streamEnded) {
      digitalWrite(AMP_SD, LOW); // 앰프 전원 완전 차단
      audio.stopSong();

      // ✅ [추가] 비행기 모드 해제: 다음 작업을 위해 WiFi 복구
      Serial.println("[AUDIO] Playback finished. Restoring WiFi...");
      setupWiFi();

      currentState = STATE_AWAKE_IDLE;
      eyes.setMood(DEFAULT);
      awakeStartTime = currentMillis; // 오디오 종료 후부터 10분 셀 카운트 시작
      isAudioFinished = false;
    }
  }

  else if (currentState == STATE_WAITING_ALARM) {
    // 1초마다 알람 시간 도달 여부만 조용하게 체크
    if (currentMillis - lastDisplayUpdate > 1000) {
      lastDisplayUpdate = currentMillis;
      checkAlarmTime();
    }
  }

  // [3] 눈동자 & 대형 시계 교대 출력 (오직 대기 상태일 때만)
  if (currentState == STATE_AWAKE_IDLE) {
    unsigned long cycle = (currentMillis - awakeStartTime) % 10000;

    if (cycle < 7000) {
      // --- 0~7초: 오직 눈동자만 렌더링 (텍스트 절대 출력 금지) ---
      eyes.update();
    } else {
      // --- 7~10초: 화면 지우고 대형 시계만 렌더링 ---
      // eyes.update()를 호출하지 않으므로 눈동자는 멈추고 숨겨짐
      display.clearDisplay();

      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {
        display.setTextSize(4); // 화면에 꽉 차는 대형 사이즈 (높이 약 28px)
        display.setTextColor(SSD1306_WHITE);

        // 128x64 해상도 정중앙에 맞추기 위한 대략적인 좌표 (HH:MM 기준)
        display.setCursor(8, 18);
        display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      }
      display.display(); // 한 번만 송출해서 깜빡임 방지
    }
  } else if (currentState ==
             STATE_WAITING_ALARM) { // 알람 대기 상태도 렌더링 (오차 방지)
    eyes.update();
  }
  // ※ 핵심 최적화: currentState == STATE_PLAYING_AUDIO 일 때는 절대 화면을
  // 건드리지 않음 (CPU 올인)

  // [4] 10분 대기 타이머 만료 시 딥슬립 돌입
  if (currentState == STATE_WAITING_ALARM || currentState == STATE_AWAKE_IDLE) {
    if (currentMillis - awakeStartTime > ACTIVE_TIME_MS) {
      calculateAndEnterDeepSleep();
    }
  }

  // 3. 루프 프레임 안정화: 오디오 재생 중에는 딜레이 없이 루프를 원천 가동하여 I2S 스트리밍 보장
  // 재생 중이 아닐 때만 미세 지연을 주어 WiFi 등 백그라운드 태스크에 CPU 양보
  if (currentState != STATE_PLAYING_AUDIO) {
    vTaskDelay(1); // yield() 보다 명시적인 스케줄러 양보
  }
}
