# 🤖 ESP32-C3 AI Bot '로미'

> ESP32-C3 기반 AI 보이스 알람 로봇 — n8n + TTS + OLED 눈동자 + 딥슬립 완전 통합

---

## 🚀 시작 전 필수 설정

업로드 전 `ESP32_C3_Bot.ino` 파일에서 아래 **3곳**을 본인 정보로 반드시 수정해야 합니다.

| 줄 번호 | 변수명 | 입력할 내용 | 예시 |
|---|---|---|---|
| **39번** | `ssid` | 연결할 Wi-Fi 이름 (SSID) | `"MyHomeWiFi"` |
| **40번** | `password` | Wi-Fi 비밀번호 | `"mypassword123"` |
| **42번** | `webhook_url` | n8n 웹훅 전체 URL | `"https://xxx.app.n8n.cloud/webhook/yyy"` |

```cpp
// ── 네트워크 설정 ──────────────────────
// ⚠️ TODO: 아래에 본인의 Wi-Fi SSID와 비밀번호를 입력하세요.
const char *ssid = "YOUR_WIFI_SSID";       // ← 39번 줄: Wi-Fi 이름
const char *password = "YOUR_WIFI_PASSWORD"; // ← 40번 줄: Wi-Fi 비밀번호
// ⚠️ TODO: 아래에 본인의 n8n 웹훅 URL을 입력하세요.
const String webhook_url = "https://YOUR_N8N_DOMAIN/webhook/YOUR_WEBHOOK_ID"; // ← 42번 줄
```

> 💡 보안을 위해 실제 값은 별도의 `secrets.h` 파일로 분리하고 `.gitignore`에 등록하는 것을 권장합니다.

---

## 📖 프로젝트 개요

**로미(Romi)**는 ESP32-C3 SuperMini 보드 위에서 동작하는 AI 보이스 알람 봇입니다.  
정해진 알람 시간(오전 7:30 / 오후 7:00)에 자동으로 깨어나거나, 버튼을 누르면 즉시 활성화됩니다.  
n8n 클라우드 웹훅을 통해 AI가 생성한 TTS(Text-to-Speech) 음성을 다운로드한 뒤, I2S 앰프로 재생합니다.  
OLED 디스플레이에는 **RoboEyes** 라이브러리로 구현된 감정 표현 눈동자가 실시간으로 움직입니다.

---

## 🏗️ 시스템 아키텍처

```
[버튼 / 알람 타이머]
        │
        ▼
[ESP32-C3 SuperMini]
   ├─ WiFi 연결 (WPA2, 8.5dBm 고정)
   ├─ NTP 시간 동기화 (KST +9)
   ├─ n8n 웹훅 HTTP GET → MP3 다운로드 → LittleFS 저장
   ├─ I2S DAC (MAX98357A 등) → 스피커 재생
   ├─ OLED 128×64 → 눈동자 / 대형 시계 교대 출력
   └─ 딥슬립 → 다음 알람 5분 전 자동 기상
```

---

## 🔩 하드웨어 구성

| 컴포넌트 | 설명 |
|---|---|
| **MCU** | ESP32-C3 SuperMini (Single-Core, 160MHz) |
| **디스플레이** | SSD1306 OLED 128×64 (I2C) |
| **오디오** | I2S DAC 앰프 모듈 (MAX98357A 계열) |
| **입력** | 택트 버튼 (GPIO 4, Active LOW) |
| **전원** | USB 5V 또는 배터리 |

### 핀맵

| 기능 | GPIO |
|---|---|
| I2S BCLK | 2 |
| I2S LRC (WS) | 3 |
| I2S DOUT | 1 |
| 앰프 SD (셧다운) | 10 |
| OLED SDA | 8 |
| OLED SCL | 9 |
| 버튼 | 4 |

---

## 📦 사용 라이브러리

아두이노 IDE 또는 PlatformIO에서 아래 라이브러리를 설치해야 합니다.

| 라이브러리 | 용도 |
|---|---|
| `Adafruit GFX Library` | 디스플레이 그래픽 기반 |
| `Adafruit SSD1306` | OLED 드라이버 |
| `ESP32-audioI2S` | I2S MP3 스트리밍/재생 |
| `FluxGarage_RoboEyes` | 눈동자 애니메이션 |
| `LittleFS` | 내장 플래시 파일 시스템 |
| `WiFi` / `WiFiClientSecure` | 네트워크 연결 |
| `HTTPClient` | 웹훅 HTTP 요청 |
| `esp_sleep` | 딥슬립 제어 |
| `time.h` | NTP 시간 동기화 |

---

## ⚙️ 아두이노 IDE 설정

1. **보드 패키지 설치**
   - Arduino IDE → `파일` → `환경설정` → 추가 보드 관리자 URL에 아래 추가:
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - `툴` → `보드` → `보드 관리자` → `esp32` 검색 후 설치

2. **보드 선택**
   - `툴` → `보드` → `ESP32C3 Dev Module`

3. **업로드 설정 (권장)**
   | 항목 | 설정값 |
   |---|---|
   | CPU Frequency | 160MHz |
   | Flash Size | 4MB |
   | Partition Scheme | Default (with SPIFFS) |
   | USB CDC On Boot | Enabled |

4. **파일시스템 업로드**
   - 오디오 파일을 `data/` 폴더에 넣고 LittleFS 업로더 플러그인으로 업로드

---

## 🔄 동작 흐름 (State Machine)

```
부팅
 └─ WiFi 연결 → NTP 시간 동기화
      │
      ├─ [버튼 기상 / 최초 부팅] ──→ STATE_AWAKE_IDLE
      │                                  └─ 10분 대기 → 딥슬립
      │
      └─ [타이머 기상] ──→ STATE_WAITING_ALARM
                              └─ 07:30 / 19:00 정시 체크
                                   └─ triggerWebhook()
                                        ├─ STATE_THINKING  (n8n MP3 다운로드)
                                        └─ STATE_PLAYING_AUDIO (I2S 재생)
                                             └─ 재생 완료 → WiFi 복구 → STATE_AWAKE_IDLE
```

### 알람 스케줄

| 기상 타이머 | 알람 실행 시간 | 설명 |
|---|---|---|
| 07:25 | **07:30:00** | 아침 알람 |
| 18:55 | **19:00:00** | 저녁 알람 |

> 딥슬립에서 알람 **5분 전**에 미리 깨어나 WiFi 연결 준비를 완료합니다.

---

## 🛰️ n8n 웹훅 연동

로미는 버튼이 눌리거나 알람 시간이 되면 n8n 클라우드 웹훅으로 HTTP GET 요청을 보냅니다.  
n8n 워크플로우에서는 AI 텍스트 생성 + TTS 변환 후 **MP3 바이너리를 응답으로 반환**해야 합니다.

```
ESP32 → GET https://<your-n8n-domain>/webhook/<id>
              ← 응답: MP3 바이너리 (Content-Type: audio/mpeg)
```

- 다운로드된 MP3는 `/tts.mp3` 경로로 LittleFS에 저장됩니다.
- HTTP 타임아웃: **45초** (TTS 생성 지연 대비)
- 실패 시 최대 **3회 자동 재시도**

---

## 📁 프로젝트 파일 구조

```
5_esp32_bot/
├── ESP32_C3_Bot.ino     # 메인 아두이노 소스 코드
├── securities.md        # 보안 취약점 분석 보고서
└── README.md            # 이 파일
```

---

## ⚠️ 보안 주의사항

이 프로젝트는 개인/학습용 목적으로 작성되었습니다.  
소스코드를 **GitHub 등 퍼블릭 저장소에 공개할 경우** 반드시 아래 사항을 확인하세요.

> **자세한 내용은 [`securities.md`](./securities.md) 를 반드시 읽어주세요.**

| # | 취약점 | 위험도 | 조치 |
|---|---|---|---|
| 1 | **Wi-Fi 비밀번호 하드코딩** | 🔴 높음 | `secrets.h` 분리 + `.gitignore` 등록 필수 |
| 2 | **웹훅 URL 하드코딩** | 🟡 중간 | API 키 헤더 추가, URL 환경변수 분리 권장 |

### `.gitignore` 권장 설정

공개 배포 전 반드시 `secrets.h` 파일을 분리하고 아래를 추가하세요:

```gitignore
# 민감한 자격 증명 파일 버전 관리 제외
secrets.h
```

---

## 🛠️ 주요 기술 특이사항

- **WiFi TX Power 8.5dBm 고정**: ESP32-C3 SuperMini 보드의 하드웨어 결함(status=6 프리징)을 원천 차단하기 위한 패치
- **WPA3 명시적 거부**: `WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK)` — WPA3 충돌 방어
- **비행기 모드 전환**: MP3 재생 중 WiFi를 완전히 OFF하여 I2S 노이즈 및 CPU 간섭 원천 차단
- **싱글코어 Non-blocking 루프**: FreeRTOS 멀티태스킹 없이 단일 루프로 오디오/OLED/버튼/슬립 통합 관리
- **자기 치유 재부팅**: WiFi 20초 타임아웃 / 시간 동기화 실패 시 `ESP.restart()` 자동 복구

---

## 📝 라이선스

This project is for personal/educational use. All rights reserved.
