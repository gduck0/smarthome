#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <wiringPi.h>
#include <wiringPiSPI.h>
#include <mqueue.h>
#include <math.h>

// ========================================
// [핀 설정 - BCM]
// ========================================
#define PIN_FIRE    6       // 불꽃 감지
#define PIN_BUZZER  20      // 능동 부저

// [ADXL345 SPI 설정]
#define SPI_CH      0
#define SPI_SPEED   1000000
#define DATAX0      0x32
#define POWER_CTL   0x2D
#define DATA_FORMAT 0x31
#define BW_RATE     0x2C

// [IPC 설정]
#define MQ_NAME         "/smarthome_mq"
#define PRIO_EMERGENCY  2

// [센서 임계값]
#define FIRE_DETECT_LOW     0       // 불꽃 감지 (Active LOW)
#define QUAKE_THRESHOLD     150     // 지진 감지 임계값
#define GRAVITY_BASELINE    256     // 중력 가속도 기준값 (1g ≈ 256)

// [상태 관리]
int fire_alarm_active = 0;
int quake_alarm_active = 0;
mqd_t mq = (mqd_t)-1;

// ========================================
// [함수: 독립적 부저 알림]
// ========================================
void trigger_alarm(int type) {
    if (type == 1) { // 화재
        printf("\n🔥 [Safety] 화재 발생! 부저 작동\n");
        for (int i = 0; i < 5; i++) {
            digitalWrite(PIN_BUZZER, HIGH);
            delay(100);
            digitalWrite(PIN_BUZZER, LOW);
            delay(100);
        }
    }
    else if (type == 2) { // 지진
        printf("\n🌊 [Safety] 지진 발생! 부저 작동\n");
        digitalWrite(PIN_BUZZER, HIGH);
        delay(1000);
        digitalWrite(PIN_BUZZER, LOW);
    }
}

// ========================================
// [함수: SPI 통신 헬퍼]
// ========================================
void writeReg(char addr, char val) {
    unsigned char buff[2] = { addr, val };
    wiringPiSPIDataRW(SPI_CH, buff, 2);
}

void readReg(char addr, int num, char* buf) {
    buf[0] = 0x80 | addr;           // Read bit
    if (num > 1) buf[0] |= 0x40;    // Multi-byte bit
    wiringPiSPIDataRW(SPI_CH, (unsigned char*)buf, num + 1);
}

// ========================================
// [함수: ADXL345 초기화]
// ========================================
int init_adxl345() {
    printf("[disaster_monitor] ADXL345 초기화 중...\n");

    // 데이터 포맷: ±4g, Full resolution
    writeReg(DATA_FORMAT, 0x01);
    delay(10);

    // 샘플링 레이트: 100Hz
    writeReg(BW_RATE, 0x0A);
    delay(10);

    // 측정 모드 활성화
    writeReg(POWER_CTL, 0x08);
    delay(100);  // 안정화 대기

    // 테스트 읽기
    char buf[7] = { 0 };
    readReg(DATAX0, 6, buf);

    if (buf[1] == 0 && buf[2] == 0 && buf[3] == 0 &&
        buf[4] == 0 && buf[5] == 0 && buf[6] == 0) {
        printf("[Error] ADXL345 응답 없음 - SPI 연결 확인 필요\n");
        return 0;
    }

    printf("[disaster_monitor] ✔ ADXL345 초기화 완료\n");
    return 1;
}

// ========================================
// [함수: 가속도 읽기 및 필터링]
// ========================================
double read_acceleration_magnitude() {
    unsigned char buf[7] = { 0 };
    readReg(DATAX0, 6, (char*)buf);

    short x = ((short)buf[2] << 8) | (short)buf[1];
    short y = ((short)buf[4] << 8) | (short)buf[3];
    short z = ((short)buf[6] << 8) | (short)buf[5];

    return sqrt(pow(x, 2) + pow(y, 2) + pow(z, 2));
}

// ========================================
// [함수: MQ 메시지 전송]
// ========================================
int send_emergency(const char* msg) {
    if (mq == (mqd_t)-1) {
        printf("[Warning] MQ not opened\n");
        return 0;
    }

    if (mq_send(mq, msg, strlen(msg) + 1, PRIO_EMERGENCY) == 0) {
        return 1;
    }
    else {
        printf("[Error] MQ send failed: %s\n", strerror(errno));
        return 0;
    }
}

// ========================================
// [함수: 정상 종료 핸들러]
// ========================================
void shutdown_handler(int signum) {
    printf("\n[disaster_monitor] 종료 신호 수신 (signal: %d)\n", signum);

    digitalWrite(PIN_BUZZER, LOW);

    if (mq != (mqd_t)-1) {
        mq_close(mq);
    }

    printf("[disaster_monitor] 정리 완료\n");
    exit(0);
}

// ========================================
// [Main]
// ========================================
int main() {
    printf("[disaster_monitor] ========================================\n");
    printf("[disaster_monitor] 안전 감시 시스템 시작\n");
    printf("[disaster_monitor] ========================================\n");

    // 시그널 핸들러 등록
    signal(SIGINT, shutdown_handler);
    signal(SIGTERM, shutdown_handler);

    // 1. GPIO 초기화
    if (wiringPiSetupGpio() == -1) {
        printf("[Error] wiringPi 초기화 실패\n");
        return 1;
    }

    // 2. SPI 초기화
    if (wiringPiSPISetupMode(SPI_CH, SPI_SPEED, 3) == -1) {
        printf("[Error] SPI 초기화 실패\n");
        return 1;
    }

    // 3. 핀 모드 설정
    pinMode(PIN_FIRE, INPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);

    // 4. ADXL345 초기화
    if (!init_adxl345()) {
        printf("[Warning] ADXL345 초기화 실패 - 지진 감지 비활성화\n");
    }

    // 5. 메시지 큐 연결 (여러 번 시도)
    for (int i = 0; i < 5; i++) {
        mq = mq_open(MQ_NAME, O_WRONLY);
        if (mq != (mqd_t)-1) {
            printf("[disaster_monitor] ✔ MQ 연결 성공\n");
            break;
        }

        printf("[disaster_monitor] MQ 연결 대기 중... (%d/5)\n", i + 1);
        sleep(1);
    }

    if (mq == (mqd_t)-1) {
        printf("[Warning] MQ 연결 실패 - 계속 진행\n");
    }

    printf("[disaster_monitor] ✔ 안전 시스템 가동 완료\n");
    printf("[disaster_monitor] 감시 시작...\n");
    printf("----------------------------------------\n");

    // ========================================
    // 메인 감시 루프
    // ========================================
    int fire_status;
    double mag;
    int loop_count = 0;

    while (1) {
        // -----------------------------------
        // 1. 화재 감지 (불꽃 센서)
        // -----------------------------------
        fire_status = digitalRead(PIN_FIRE);

        if (fire_status == FIRE_DETECT_LOW) {
            if (!fire_alarm_active) {
                fire_alarm_active = 1;
                trigger_alarm(1);
                send_emergency("EMERGENCY_FIRE");
                printf("[disaster_monitor] 🚨 화재 경보 발령\n");
            }
            delay(1000);  // 화재 감지 중 1초 대기
        }
        else {
            if (fire_alarm_active) {
                fire_alarm_active = 0;
                printf("[disaster_monitor] ✔ 화재 상태 해제\n");
            }
        }

        // -----------------------------------
        // 2. 지진 감지 (가속도 센서)
        // -----------------------------------
        mag = read_acceleration_magnitude();
        double deviation = fabs(mag - GRAVITY_BASELINE);

        if (deviation > QUAKE_THRESHOLD) {
            if (!quake_alarm_active) {
                quake_alarm_active = 1;
                trigger_alarm(2);
                send_emergency("EMERGENCY_QUAKE");
                printf("[disaster_monitor] 🚨 지진 감지! (편차: %.0f)\n", deviation);
            }
            delay(1000);  // 지진 감지 중 1초 대기
        }
        else {
            if (quake_alarm_active) {
                quake_alarm_active = 0;
                printf("[disaster_monitor] ✔ 지진 상태 해제\n");
            }
        }

        // -----------------------------------
        // 3. 주기적 상태 출력 (30초마다)
        // -----------------------------------
        loop_count++;
        if (loop_count % 150 == 0) {  // 150 * 200ms = 30초
            printf("[disaster_monitor] ♥ 정상 동작 중 (화재: %s, 지진: %s, 가속도편차: %.0f)\n",
                fire_alarm_active ? "감지" : "정상",
                quake_alarm_active ? "감지" : "정상",
                deviation);
            loop_count = 0;
        }

        // 센서 읽기 주기
        delay(200);  // 200ms
    }

    // 정리 (실제로는 도달 안 함)
    if (mq != (mqd_t)-1) {
        mq_close(mq);
    }

    return 0;
}
