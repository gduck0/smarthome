# 스마트홈 통합 제어 시스템

라즈베리파이 기반 스마트홈. 멀티프로세스로 분리해서 영상/클라우드, 안전 감시, 메인 제어(액추에이터·센서·NFC·블루투스)를 동시에 돌린다.

## 구조

```
launcher (부모)
  ├─ fork → main_controller (C, 메인 제어)
  ├─ fork → vision_uploader (Python, 영상/클라우드)
  └─ fork → disaster_monitor (C, 재난 감시)
```

launcher가 세 프로세스를 순서대로 fork/exec 한다. main_controller를 가장 먼저 띄우는 이유는 거기서 POSIX 메시지 큐(`/smarthome_mq`)를 생성해야 disaster_monitor가 거기에 붙을 수 있기 때문이다. main_controller 띄우고 3초, vision_uploader 띄우고 2초씩 대기 후 다음 프로세스를 띄운다.

launcher는 띄운 다음 `wait()`로 자식들을 감시한다. 셋 중 하나라도 죽으면 어떤 프로세스가 죽었는지 출력하고 점검 명령어를 안내한다. 자동 재시작은 안 하고 알림만 한다.

프로세스 간 통신은 메시지 큐 하나로 다 처리한다. disaster_monitor가 위험 감지하면 큐에 메시지 보내고, main_controller 내부 스레드(블루투스, 센서)도 같은 큐에 메시지를 보낸다. main_controller의 액추에이터 스레드가 그 큐를 받아서 실제 동작(서보, LED, 팬)을 실행한다.

## 프로세스별 역할

**main_controller.c (메인 제어)**
스레드 4개로 나눠서 동작한다.
- actuator: 메시지 큐를 받아서 서보/LED/팬 실제로 움직임. 비상 메시지 받으면 LED 깜빡이고 창문 강제 개방
- sensors: 온도/조도/수위 센서 주기적으로 읽고 자동 모드일 때 알맞은 명령을 큐에 넣음
- bluetooth: 블루투스로 들어오는 문자 명령(O/C/L/l/W/w/A/M)을 큐 메시지로 변환
- nfc: PN532 리더기로 태그 감지, 감지되면 문 열림 메시지 전송

자동/수동 모드 전환이 있고, 비상 상황 중에는 자동 모드 로직이 무시된다.

**vision_uploader.py (영상/클라우드)**
초음파 센서로 30cm 이내 접근을 감지하면 카메라로 사진을 찍고 rclone으로 Google Drive에 업로드한다. 5초 쿨다운이 있어서 연속 트리거를 막는다.

**disaster_monitor.c (재난 감시)**
불꽃 센서(화재)와 ADXL345 가속도 센서(지진)를 감시한다. 감지되면 부저를 울리고 메시지 큐로 메인 제어에 `EMERGENCY_FIRE` / `EMERGENCY_QUAKE`를 보낸다. MQ 연결은 launcher 타이밍 문제로 바로 안 될 수 있어서 5번 재시도한다.

## 기술 스택

- C: wiringPi, wiringPiSPI, wiringPiI2C, POSIX message queue, pthread
- Python: picamera2, lgpio, rclone(subprocess)
- 센서: 초음파, 불꽃, ADXL345(SPI), 온도/조도/수위(I2C ADC)
- 통신: 블루투스 시리얼, NFC(PN532, UART)

## 겪었던 문제

**프로세스 시작 순서**
disaster_monitor가 main_controller보다 먼저 뜨면 메시지 큐가 없어서 접속에 실패한다. launcher에서 main_controller를 먼저 fork하고 sleep(3)으로 MQ 생성 시간을 확보한 다음 vision_uploader, disaster_monitor를 순서대로 띄우게 했다. disaster_monitor 쪽에도 5번 재시도 로직을 넣어서 한 번 더 방어했다.

**LED 제어 충돌**
LED를 PWM(조도 자동 조절)과 디지털 출력(비상 깜빡임) 두 가지 방식으로 같이 쓰다 보니 충돌이 났다. 비상 상황 진입 시 `softPwmStop`으로 PWM을 끄고 `pinMode`로 출력 모드를 전환한 다음, 상황 종료되면 다시 `softPwmCreate`로 복구하는 방식으로 분리했다.

**NFC 초기화**
PN532 모듈이 SAMConfiguration 응답을 못 받는 경우가 있어서 wakeup 시퀀스와 버퍼 flush를 추가했다. 태그 감지 판별은 응답 프레임에서 `0xD5 0x4B` 패턴만 찾는 단순한 방식으로 처리했다.

## 실행

```
gcc -o main_controller main_controller.c -lwiringPi -lpthread -lm -lrt
gcc -o disaster_monitor disaster_monitor.c -lwiringPi -lm -lrt
gcc -o launcher launcher.c
./launcher
```

`vision_uploader.py`는 launcher가 `python3 vision_uploader.py`로 직접 실행하므로 따로 컴파일할 필요 없다.

## 미완성 / 한계

- NFC 스레드는 SAMConfiguration 검증까지는 동작하나 실제 환경에서 안정성 검증이 부족함
- launcher의 자식 프로세스 재시작 로직 없음 (감지만 하고 알림만 띄움)
- 센서 임계값(온도, 조도, 거리 등)이 하드코딩되어 있어 환경 바뀌면 코드 수정 필요
