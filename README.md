# 스마트홈 통합 제어 시스템

Raspberry Pi에서 센서·액추에이터·NFC·Bluetooth·카메라를 함께 제어하는 스마트홈 프로젝트입니다. 메인 제어, 재난 감시, 영상 처리 기능을 별도 프로세스로 분리하고 POSIX Message Queue와 pthread를 이용해 여러 하드웨어 이벤트를 처리합니다.

## 프로젝트 정보

- 기간: 2025.11 ~ 2025.12
- 형태: 4인 팀 임베디드 시스템 최종 프로젝트
- 주요 구성: Raspberry Pi · C · Python · POSIX Message Queue · pthread
- 원본 팀 저장소: https://github.com/hyemniJ/Team1

## 구조

```text
launcher
  ├─ main_controller      메인 제어
  ├─ vision_uploader      카메라·영상 처리
  └─ disaster_monitor     화재·지진 감시
```

`launcher`가 `main_controller → vision_uploader → disaster_monitor` 순서로 프로세스를 실행합니다. `main_controller`가 먼저 POSIX Message Queue(`/smarthome_mq`)를 생성하고, 다른 프로세스가 필요한 이벤트를 메시지 큐로 전달합니다.

```text
센서 / Bluetooth / NFC / 재난 감지
              ↓
       POSIX Message Queue
              ↓
        actuator thread
              ↓
       Servo / LED / Fan
```

## 담당 역할

### 본인 — 메인 제어 / 재난 감시

**Main Controller**
- `main_controller.c` 메인 제어 로직 구현
- POSIX Message Queue 생성 및 제어 이벤트 처리
- `actuator`, `sensors`, `bluetooth`, `nfc` 역할을 pthread로 분리
- 온도·조도·수위 센서 기반 자동 제어
- Bluetooth 기반 수동 제어
- PN532 NFC 입력 처리
- Servo, LED, Fan 제어
- Mutex를 이용한 액추에이터 접근 보호
- 화재·지진 비상 이벤트 우선 처리

**Disaster Monitor**
- `disaster_monitor.c` 재난 감시 로직 구현
- 불꽃 센서를 이용한 화재 감지
- ADXL345 가속도 센서를 이용한 지진 감지
- 위험 감지 시 부저 경보
- `EMERGENCY_FIRE`, `EMERGENCY_QUAKE` 메시지 전송
- POSIX Message Queue 연결 재시도 처리

## 팀 구성

- 본인: 메인 제어 프로그램, 센서·액추에이터 제어, Bluetooth/NFC 처리, 화재·지진 감시
- 팀원 1: Raspberry Pi 카메라 및 영상 처리, 접근 감지, 이미지 촬영·클라우드 업로드
- 팀원 2: 스마트홈 모형 제작, 집 구조 및 내부 배치 구성, 센서·모듈 장착 위치 설계
- 팀원 3: 하드웨어 및 회로 구성, Raspberry Pi와 센서·액추에이터 배선·연결, 하드웨어 동작 테스트

## 기술 스택

- Language: C, Python
- Concurrency: pthread, multi-process
- IPC: POSIX Message Queue
- GPIO / Sensor: wiringPi, wiringPiSPI, wiringPiI2C, lgpio
- Camera: Picamera2
- Communication: Bluetooth Serial, NFC(PN532/UART)
- Cloud: rclone, Google Drive
- Device: Raspberry Pi

## 폴더

```text
smarthome/
├── launcher.c           전체 프로세스 실행 및 종료 감시
├── main_controller.c    센서·액추에이터·Bluetooth·NFC 통합 제어
├── disaster_monitor.c   화재·지진 감시
├── vision_uploader.py   접근 감지·카메라 촬영·클라우드 업로드
└── README.md
```

## 주요 기능

- 온도에 따른 Fan 자동 제어
- 조도에 따른 LED 제어
- 수위 감지 기반 창문 제어
- Bluetooth 기반 수동 제어
- NFC 기반 출입 동작
- 화재·지진 감지 및 비상 동작
- 접근 감지 후 방문자 이미지 촬영 및 Google Drive 업로드
- 부모 프로세스에서 각 기능 프로세스 종료 상태 확인

## 프로세스별 동작

### `main_controller.c`

4개의 pthread로 역할을 분리합니다.

- `actuator`: 메시지 큐에서 명령을 받아 Servo, LED, Fan 제어
- `sensors`: 온도·조도·수위 센서를 읽고 자동 제어 명령 생성
- `bluetooth`: Bluetooth 시리얼 명령을 메시지 큐 이벤트로 변환
- `nfc`: PN532 태그를 감지해 출입 제어 이벤트 생성

### `disaster_monitor.c`

- 불꽃 센서로 화재 상태 감시
- ADXL345 가속도 센서로 지진 상태 감시
- 위험 감지 시 부저 동작
- 비상 메시지를 메인 제어 프로세스로 전달
- 메시지 큐가 아직 준비되지 않은 경우 연결 재시도

### `vision_uploader.py`

- 초음파 센서로 약 30cm 이내 접근 감지
- Picamera2로 방문자 이미지 촬영
- `rclone`을 이용해 Google Drive로 업로드
- 5초 쿨다운으로 짧은 시간 안의 중복 촬영 방지

## 겪었던 문제

**프로세스 시작 순서와 메시지 큐 연결**  
`disaster_monitor`가 `main_controller`보다 먼저 실행되면 `/smarthome_mq`가 아직 생성되지 않아 연결에 실패할 수 있었습니다. `launcher`에서 `main_controller`를 먼저 실행하고 초기화 시간을 확보한 다음 다른 프로세스를 실행하도록 순서를 정했으며, `disaster_monitor`에도 메시지 큐 연결 재시도 로직을 추가했습니다.

**LED 제어 방식 충돌**  
조도 자동 제어에서는 PWM을 사용하고 비상 경보에서는 디지털 출력을 사용해 동일한 LED를 두 방식으로 제어할 때 충돌이 발생했습니다. 비상 진입 시 `softPwmStop()`으로 PWM을 종료하고 GPIO 출력으로 전환한 뒤, 비상 해제 시 `softPwmCreate()`로 PWM을 다시 초기화하도록 처리했습니다.

**PN532 NFC 초기화 불안정**  
PN532가 정상 응답하지 않는 경우가 있어 wake-up 시퀀스와 시리얼 버퍼 flush를 추가했습니다. 태그 감지는 응답 프레임의 `0xD5 0x4B` 패턴을 확인하는 방식으로 처리합니다.

## 데모

| 기능 | 영상 |
| --- | --- |
| 수동 제어 모드 | https://youtu.be/4bdONUhknAw |
| 화재 감지 | https://youtu.be/hsq4wnOimoA |
| 지진 감지 | https://youtu.be/ztc-m-baYxQ |
| 우천 시 창문 자동 제어 | https://youtu.be/2Qh40o0klbg |
| 조도 기반 스마트 조명 | https://youtu.be/e_24HbzEvnk |
| CCTV 보안 기능 | https://youtube.com/shorts/1my3A3MdJHM?feature=share |

원본 팀 저장소에는 전체 시스템 구조도, 하드웨어 연결도, 소프트웨어 구조도와 데모 영상 파일이 함께 포함되어 있습니다.

## 빌드 및 실행

Raspberry Pi/Linux 환경과 연결된 하드웨어를 전제로 합니다.

```bash
gcc -o main_controller main_controller.c -lwiringPi -lpthread -lm -lrt
gcc -o disaster_monitor disaster_monitor.c -lwiringPi -lm -lrt
gcc -o launcher launcher.c
./launcher
```

`vision_uploader.py`는 `launcher`에서 `python3 vision_uploader.py`로 실행합니다.

영상 업로드 기능을 사용하려면 환경에 맞게 다음 값을 수정해야 합니다.

- 이미지 저장 경로
- rclone 설정 파일 경로
- Google Drive remote 이름
- Raspberry Pi GPIO 및 연결 장치 구성

## 현재 한계

- NFC 기능은 실제 환경에서의 장시간 안정성 검증이 부족합니다.
- `launcher`는 자식 프로세스 종료를 감지하지만 자동 재시작하지 않습니다.
- 센서 임계값과 일부 장치 경로가 코드에 직접 설정되어 있어 다른 환경에서는 수정이 필요합니다.
- 하드웨어 의존성이 높아 일반 PC 환경에서는 전체 기능을 그대로 재현하기 어렵습니다.
