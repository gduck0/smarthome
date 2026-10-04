#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <pthread.h>
#include <wiringPi.h>
#include <wiringSerial.h>
#include <wiringPiI2C.h>
#include <softPwm.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>

// [핀 설정]
#define PIN_SERVO   24
#define PIN_SERVO_W 22
#define PIN_LED     27
#define PIN_FAN_A   19
#define PIN_FAN_B   23
#define PIN_TRIG    15  
#define PIN_ECHO    18  

// [통신]
#define BT_DEV      "/dev/ttyAMA2"
#define BT_BAUD     115200
#define NFC_DEV     "/dev/ttyAMA4"
#define NFC_BAUD    115200

#define ADC_ADDR    0x48
#define CH_TEMP     1
#define CH_LIGHT    0
#define CH_WATER    2

#define MQ_NAME     "/smarthome_mq"
#define BUF_SIZE    256
#define PRIO_EMERGENCY 2
#define PRIO_NORMAL    1

#define WIN_OPEN    15
#define WIN_CLOSE   5

// PN532 명령어
#define PN532_PREAMBLE      0x00
#define PN532_STARTCODE1    0x00
#define PN532_STARTCODE2    0xFF
#define PN532_POSTAMBLE     0x00
#define PN532_COMMAND_SAMCONFIGURATION    0x14
#define PN532_COMMAND_INLISTPASSIVETARGET 0x4A

// 온도 설정
#define TEMP_HIGH   28.0
#define TEMP_LOW    25.0
#define TEMP_HYSTERESIS 1.0

pthread_mutex_t mutex_lock;
int bt_fd = -1;
int i2c_fd = -1;
int nfc_fd = -1;
pid_t pid_a = 0;

int autoMode = 1;
int emergency_mode = 0;
time_t last_ultrasonic_trigger = 0;

// 상태 변수
int last_water_state = -1;
int last_fan_state = -1;
int last_led_level = -1;

// 함수 선언
void init_hardware();
void find_vision_uploader_pid();
void* thread_actuator(void* arg);
void* thread_bluetooth(void* arg);
void* thread_sensors(void* arg);
void* thread_nfc(void* arg);

// NFC 유틸
unsigned char calculate_checksum(unsigned char* data, int len) {
    unsigned char sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return ~sum + 1;
}

void nfc_send_command(unsigned char* cmd, int len) {
    if (nfc_fd == -1) return;
    unsigned char frame[256];
    int idx = 0;
    frame[idx++] = PN532_PREAMBLE;
    frame[idx++] = PN532_STARTCODE1;
    frame[idx++] = PN532_STARTCODE2;
    frame[idx++] = len;
    frame[idx++] = (~len + 1) & 0xFF;
    for (int i = 0; i < len; i++) frame[idx++] = cmd[i];
    frame[idx++] = calculate_checksum(cmd, len);
    frame[idx++] = PN532_POSTAMBLE;
    write(nfc_fd, frame, idx);
}

void nfc_flush_buffer() {
    if (nfc_fd == -1) return;
    while (serialDataAvail(nfc_fd) > 0) serialGetchar(nfc_fd);
}

void nfc_wakeup() {
    if (nfc_fd == -1) return;
    unsigned char wake[] = { 0x55, 0x55, 0x00, 0x00, 0x00 };
    write(nfc_fd, wake, sizeof(wake));
    delay(100);
    nfc_flush_buffer();
}

int nfc_read_response(unsigned char* buf, int max_len, int timeout_ms) {
    if (nfc_fd == -1) return 0;
    int idx = 0;
    long start = millis();
    while (millis() - start < timeout_ms) {
        if (serialDataAvail(nfc_fd)) {
            buf[idx++] = serialGetchar(nfc_fd);
            if (idx >= max_len) break;
            start = millis();
        }
        delay(1);
    }
    return idx;
}

// ADC 읽기
int read_adc_stable(int channel) {
    if (i2c_fd == -1) return 0;
    int sum = 0;
    int valid_reads = 0;
    for (int i = 0; i < 5; i++) {
        wiringPiI2CWrite(i2c_fd, 0x40 | (channel & 0x03));
        usleep(5000);
        wiringPiI2CRead(i2c_fd);
        int val = wiringPiI2CRead(i2c_fd);
        if (val >= 0 && val <= 255) {
            sum += val;
            valid_reads++;
        }
        usleep(1000);
    }
    return valid_reads > 0 ? sum / valid_reads : 0;
}

float get_temperature() {
    return (float)read_adc_stable(CH_TEMP);
}

int get_water_sensor() { return read_adc_stable(CH_WATER); }
int get_light_sensor() { return read_adc_stable(CH_LIGHT); }


int main(int argc, char** argv) {
    printf("=== [main_controller] 통합 제어 시스템 시작 ===\n");

    if (wiringPiSetupGpio() == -1) {
        printf("[main_controller] ❌ wiringPi 초기화 실패\n");
        return 1;
    }
    printf("[main_controller] ✔ GPIO 초기화 완료\n");

    // ==========================================================
    // MQ 생성을 최우선으로 실행
    // 하드웨어 초기화보다 먼저 수행하여 Process B가 접속할 수 있게 함
    // ==========================================================
    mq_unlink(MQ_NAME); // 기존 큐 제거
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = BUF_SIZE;

    // O_CREAT: 생성, O_RDWR: 읽기쓰기
    mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, 0644, &attr);
    if (mq == (mqd_t)-1) {
        perror("[main_controller] MQ 생성 실패");
        return 1;
    }
    printf("[main_controller] ✔ 메시지 큐 생성 완료 (즉시 사용 가능)\n");

    // 메시지 큐 생성 후 하드웨어 초기화
    init_hardware();

    // vision_uploader의 PID 찾기 (최대 10초 대기 - launcher가 늦게 켜도 기다려줌)
    printf("[main_controller] vision_uploader 검색 중...\n");
    for (int i = 0; i < 10; i++) {
        find_vision_uploader_pid();
        if (pid_a > 0) {
            printf("[main_controller] ✔ vision_uploader 발견 (PID: %d)\n", pid_a);
            break;
        }
        printf("[main_controller] 대기 중... (%d/10)\n", i + 1);
        sleep(1);
    }

    if (pid_a == 0) {
        printf("[main_controller] ⚠️  vision_uploader를 찾을 수 없습니다. 카메라 기능 비활성화.\n");
    }

    pthread_mutex_init(&mutex_lock, NULL);

    pthread_t t_act, t_bt, t_sens, t_nfc;

    // 스레드 인자로 MQ 전달
    mqd_t* mq_ptr;

    mq_ptr = malloc(sizeof(mqd_t)); *mq_ptr = mq;
    pthread_create(&t_act, NULL, thread_actuator, (void*)mq_ptr);

    mq_ptr = malloc(sizeof(mqd_t)); *mq_ptr = mq;
    pthread_create(&t_bt, NULL, thread_bluetooth, (void*)mq_ptr);

    mq_ptr = malloc(sizeof(mqd_t)); *mq_ptr = mq;
    pthread_create(&t_sens, NULL, thread_sensors, (void*)mq_ptr);

    mq_ptr = malloc(sizeof(mqd_t)); *mq_ptr = mq;
    pthread_create(&t_nfc, NULL, thread_nfc, (void*)mq_ptr);

    printf("[main_controller] ✔ 모든 스레드 시작 완료\n");
    printf("[main_controller] ==========================================\n\n");

    pthread_join(t_act, NULL); // 메인 스레드는 액추에이터 스레드가 끝날 때까지 대기

    // 종료 처리
    if (bt_fd >= 0) serialClose(bt_fd);
    if (nfc_fd >= 0) serialClose(nfc_fd);
    mq_close(mq);
    mq_unlink(MQ_NAME);
    pthread_mutex_destroy(&mutex_lock);
    return 0;
}

// [Thread 1] 액추에이터
void* thread_actuator(void* arg) {
    mqd_t mq = *((mqd_t*)arg);
    free(arg);
    char buf[BUF_SIZE];
    unsigned int prio;

    while (1) {
        ssize_t r = mq_receive(mq, buf, BUF_SIZE, &prio);
        if (r >= 0) {
            buf[r] = '\0';
            printf(" >> [Actuator] %s\n", buf);
            pthread_mutex_lock(&mutex_lock);

            // 1. 비상 상황 처리
            if (strstr(buf, "EMERGENCY") != NULL) {
                emergency_mode = 1;
                autoMode = 0;

                printf(" 🚨 [비상] LED 경보\n");

                /* ===== LED 제어 권한 단일화 ===== */
                softPwmStop(PIN_LED);
                pinMode(PIN_LED, OUTPUT);

                /* 경보 깜빡임 */
                for (int i = 0; i < 5; i++) {
                    digitalWrite(PIN_LED, HIGH); delay(150);
                    digitalWrite(PIN_LED, LOW);  delay(150);
                }

                /* 비상 상태 유지 */
                digitalWrite(PIN_LED, HIGH);

                /* 창문 / 문 개방 */
                softPwmWrite(PIN_SERVO, WIN_OPEN);
                softPwmWrite(PIN_SERVO_W, WIN_OPEN);

                pthread_mutex_unlock(&mutex_lock);
                sleep(5);
                pthread_mutex_lock(&mutex_lock);

                /* ===== 비상 해제 ===== */
                digitalWrite(PIN_LED, LOW);
                softPwmCreate(PIN_LED, 0, 100);

                autoMode = 1;
                emergency_mode = 0;
            }
                // 2. NFC 문 열림
            else if (strcmp(buf, "NFC_OPEN_DOOR") == 0) {
                printf(" [NFC] 문 열림 동작\n");
                for (int i = 0; i < 3; i++) {
                    digitalWrite(PIN_LED, HIGH); delay(100);
                    digitalWrite(PIN_LED, LOW);  delay(100);
                }

                softPwmWrite(PIN_SERVO, WIN_OPEN);
                pthread_mutex_unlock(&mutex_lock);
                sleep(3);
                pthread_mutex_lock(&mutex_lock);
                softPwmWrite(PIN_SERVO, WIN_CLOSE);
            }
            // 3. LED 제어
            else if (strncmp(buf, "AUTO_LED_", 9) == 0 || strncmp(buf, "CMD_LED_", 8) == 0) {
                if (strstr(buf, "_HIGH") || strstr(buf, "_ON")) {
                    softPwmStop(PIN_LED); pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);
                }
                else if (strstr(buf, "_MID")) {
                    softPwmCreate(PIN_LED, 0, 100); softPwmWrite(PIN_LED, 40);
                }
                else {
                    softPwmStop(PIN_LED); pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
                }
            }
            // 4. 수동/자동 기타 명령
            else if (strncmp(buf, "CMD_", 4) == 0) {
                autoMode = 0;
                if (strcmp(buf, "CMD_OPEN") == 0) softPwmWrite(PIN_SERVO, WIN_OPEN);
                else if (strcmp(buf, "CMD_CLOSE") == 0) softPwmWrite(PIN_SERVO, WIN_CLOSE);
                else if (strcmp(buf, "CMD_FAN_ON") == 0) { digitalWrite(PIN_FAN_A, HIGH); digitalWrite(PIN_FAN_B, LOW); }
                else if (strcmp(buf, "CMD_FAN_OFF") == 0) { digitalWrite(PIN_FAN_A, LOW); digitalWrite(PIN_FAN_B, LOW); }
                else if (strcmp(buf, "CMD_WIN_OPEN") == 0) softPwmWrite(PIN_SERVO_W, WIN_OPEN);
                else if (strcmp(buf, "CMD_WIN_CLOSE") == 0) softPwmWrite(PIN_SERVO_W, WIN_CLOSE);
            }
            else if (strcmp(buf, "MODE_AUTO") == 0) autoMode = 1;
            else if (strcmp(buf, "MODE_MANUAL") == 0) autoMode = 0;

            else if (strncmp(buf, "AUTO_", 5) == 0 && autoMode == 1 && emergency_mode == 0) {
                if (strcmp(buf, "AUTO_WIN_CLOSE") == 0) softPwmWrite(PIN_SERVO_W, WIN_CLOSE);
                else if (strcmp(buf, "AUTO_FAN_ON") == 0) { digitalWrite(PIN_FAN_A, HIGH); digitalWrite(PIN_FAN_B, LOW); }
                else if (strcmp(buf, "AUTO_FAN_OFF") == 0) { digitalWrite(PIN_FAN_A, LOW); digitalWrite(PIN_FAN_B, LOW); }
            }

            pthread_mutex_unlock(&mutex_lock);
        }
    }
    return NULL;
}

// [Thread 2] 센서 모니터링
void* thread_sensors(void* arg) {
    mqd_t mq = *((mqd_t*)arg);
    free(arg);

    while (1) {
        // ==========================================
        // 자동 모드 센서 처리
        // ==========================================
        if (autoMode == 1) {
            // 워터 센서
            int water_val = get_water_sensor();
            int current_water = (water_val > 100) ? 1 : 0;
            if (current_water != last_water_state) {
                if (current_water == 1) mq_send(mq, "AUTO_WIN_CLOSE", 15, PRIO_NORMAL);
                last_water_state = current_water;
            }

            // 온도 센서
            float temp = get_temperature();
            int target_fan = last_fan_state;
            if (last_fan_state == 0 && temp >= TEMP_HIGH) target_fan = 1;
            else if (last_fan_state == 1 && temp < (TEMP_LOW - TEMP_HYSTERESIS)) target_fan = 0;

            if (target_fan != last_fan_state) {
                if (target_fan == 1) mq_send(mq, "AUTO_FAN_ON", 12, PRIO_NORMAL);
                else mq_send(mq, "AUTO_FAN_OFF", 13, PRIO_NORMAL);
                last_fan_state = target_fan;
            }

            // 조도 센서
            int light_val = get_light_sensor();
            int current_level = last_led_level;

            if (light_val > 200) current_level = 3;
            else if (light_val < 60) current_level = 1;
            else if (light_val > 80 && light_val < 180) current_level = 2;

            if (last_led_level == -1) last_led_level = 0;

            if (current_level != last_led_level) {
                if (current_level == 3) mq_send(mq, "AUTO_LED_HIGH", 14, PRIO_NORMAL);
                else if (current_level == 2) mq_send(mq, "AUTO_LED_MID", 13, PRIO_NORMAL);
                else if (current_level == 1) mq_send(mq, "AUTO_LED_LOW", 13, PRIO_NORMAL);
                last_led_level = current_level;
            }
        }

        usleep(100000);  // 100ms 대기
    }
    return NULL;
}

// [Thread 3] 블루투스
void* thread_bluetooth(void* arg) {
    mqd_t mq = *((mqd_t*)arg);
    free(arg);
    if (bt_fd == -1) {
        printf("[Bluetooth] 장치 없음\n");
        return NULL;
    }
    printf("[Bluetooth] 스레드 시작\n");

    while (1) {
        if (serialDataAvail(bt_fd)) {
            char c = serialGetchar(bt_fd);
            char cmd[30] = "";
            if (c == 'O') strcpy(cmd, "CMD_OPEN");
            else if (c == 'C') strcpy(cmd, "CMD_CLOSE");
            else if (c == 'L') strcpy(cmd, "CMD_LED_ON");
            else if (c == 'l') strcpy(cmd, "CMD_LED_OFF");
            else if (c == 'W') strcpy(cmd, "CMD_WIN_OPEN");
            else if (c == 'w') strcpy(cmd, "CMD_WIN_CLOSE");
            else if (c == 'A') strcpy(cmd, "MODE_AUTO");
            else if (c == 'M') strcpy(cmd, "MODE_MANUAL");

            if (strlen(cmd) > 0) {
                mq_send(mq, cmd, strlen(cmd) + 1, PRIO_NORMAL);
                printf("[Bluetooth] 명령 수신: %c -> %s\n", c, cmd);
            }
        }
        usleep(10000);
    }
    return NULL;
}

// [Thread 4] NFC (구현 실패)
void* thread_nfc(void* arg) {
    mqd_t mq = *((mqd_t*)arg);
    free(arg);

    if (nfc_fd < 0) {
        printf("[NFC] ❌ NFC 장치 열기 실패\n");
        return NULL;
    }

    printf("[NFC] PN532 초기화 시작\n");

    // 1️⃣ Wakeup
    nfc_wakeup();
    delay(500);
    nfc_flush_buffer();

    // 2️⃣ SAMConfiguration
    unsigned char sam_cmd[] = { 0xD4, 0x14, 0x01, 0x14, 0x01 };
    nfc_send_command(sam_cmd, sizeof(sam_cmd));

    unsigned char resp[128];
    int len = nfc_read_response(resp, sizeof(resp), 300);

    // SAM 응답 검증
    int sam_ok = 0;
    for (int i = 0; i < len - 1; i++) {
        if (resp[i] == 0xD5 && resp[i + 1] == 0x15) {
            sam_ok = 1;
            break;
        }
    }

    if (!sam_ok) {
        printf("[NFC] ❌ SAMConfiguration 실패\n");
        printf("[NFC DEBUG] SAM RX (%d): ", len);
        for (int i = 0; i < len; i++) printf("%02X ", resp[i]);
        printf("\n");
        return NULL;
    }
    
    printf("[NFC] ✔ SAMConfiguration 성공\n");
    printf("[NFC] 리더기 활성화 완료\n");

    // ===============================
    // 3️⃣ 태그 감지 루프
    // ===============================
    while (1) {
        nfc_flush_buffer();

        unsigned char cmd[] = { 0xD4, 0x4A, 0x01, 0x00 }; // InListPassiveTarget
        nfc_send_command(cmd, sizeof(cmd));

        len = nfc_read_response(resp, sizeof(resp), 300);

        // 🔍 디버그 출력 (필수)
        if (len > 0) {
            printf("[NFC DEBUG] RX (%d): ", len);
            for (int i = 0; i < len; i++) printf("%02X ", resp[i]);
            printf("\n");
        }

        // 태그 감지 응답 확인
        for (int i = 0; i < len - 1; i++) {
            if (resp[i] == 0xD5 && resp[i + 1] == 0x4B) {
                printf("\n✨ [NFC] 태그 감지 성공!\n");
                mq_send(mq, "NFC_OPEN_DOOR", 14, PRIO_NORMAL);
                delay(3000);  // 중복 인식 방지
                break;
            }
        }

        delay(500);  // 폴링 주기
    }

    return NULL;
}


void init_hardware() {
    printf("[Hardware] 초기화 시작...\n");

    // 서보
    pinMode(PIN_SERVO, OUTPUT);
    softPwmCreate(PIN_SERVO, WIN_CLOSE, 200);

    pinMode(PIN_SERVO_W, OUTPUT);
    softPwmCreate(PIN_SERVO_W, WIN_CLOSE, 200);
    printf("[Hardware]  - 서보 모터 초기화 완료\n");

    // LED
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_LED, LOW);
    printf("[Hardware]  - LED 초기화 완료\n");

    // 팬
    pinMode(PIN_FAN_A, OUTPUT); pinMode(PIN_FAN_B, OUTPUT);
    digitalWrite(PIN_FAN_A, LOW); digitalWrite(PIN_FAN_B, LOW);
    printf("[Hardware]  - 팬 초기화 완료\n");

    // I2C
    if ((i2c_fd = wiringPiI2CSetup(ADC_ADDR)) < 0) {
        printf("[Hardware]  - I2C 초기화 실패\n");
    }
    else {
        printf("[Hardware]  - I2C 초기화 완료\n");
    }

    // Bluetooth
    if ((bt_fd = serialOpen(BT_DEV, BT_BAUD)) < 0) {
        printf("[Hardware]  - Bluetooth 초기화 실패\n");
    }
    else {
        printf("[Hardware]  - Bluetooth 초기화 완료\n");
    }

    // NFC
    if ((nfc_fd = serialOpen(NFC_DEV, NFC_BAUD)) < 0) {
        printf("[Hardware]  - NFC 초기화 실패\n");
    }
    else {
        printf("[Hardware]  - NFC 초기화 완료\n");
    }

    printf("[Hardware] 초기화 완료\n\n");
}

void find_vision_uploader_pid() {
    FILE* fp = popen("pgrep -f vision_uploader.py", "r");
    if (fp) {
        fscanf(fp, "%d", &pid_a);
        pclose(fp);
    }
    else {
        pid_a = 0;
    }
}
