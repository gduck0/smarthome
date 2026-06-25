#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

int main() {
    pid_t pid_A, pid_B, pid_C;

    printf("[Launcher] ========================================\n");
    printf("[Launcher] 스마트홈 통합 시스템을 시작합니다.\n");
    printf("[Launcher] ========================================\n");

    // -------------------------------------------------------
    // 1. main_controller (메인 제어 & NFC - C언어) 먼저 실행
    //    이유: 메시지 큐(MQ)를 생성해야 disaster_monitor가 접속할 수 있음
    // -------------------------------------------------------
    pid_C = fork();
    if (pid_C < 0) { perror("Fork main_controller Failed"); exit(1); }
    else if (pid_C == 0) {
        execl("./main_controller", "main_controller", NULL);
        perror("[Error] main_controller 실행 실패");
        exit(1);
    }
    printf("[Launcher] main_controller (메인 제어) 가동됨 (PID: %d)\n", pid_C);

    // 메인 제어가 MQ를 생성하고 초기화할 시간을 충분히 줌 (3초)
    sleep(3);

    // -------------------------------------------------------
    // 2. vision_uploader (영상/클라우드 - Python) 실행
    // -------------------------------------------------------
    pid_A = fork();
    if (pid_A < 0) { perror("Fork vision_uploader Failed"); exit(1); }
    else if (pid_A == 0) {
        // 파이썬 스크립트 실행: vision_uploader.py
        execlp("python3", "python3", "vision_uploader.py", NULL);
        perror("[Error] vision_uploader 실행 실패");
        exit(1);
    }
    printf("[Launcher] vision_uploader (Vision) 가동됨 (PID: %d)\n", pid_A);

    // 카메라를 켜는 동안 대기
    sleep(2);

    // -------------------------------------------------------
    // 3. disaster_monitor (재난 감시 - C언어) 실행
    //    이제 메인 제어가 켜져 있으므로 바로 MQ에 접속 가능
    // -------------------------------------------------------
    pid_B = fork();
    if (pid_B < 0) { perror("Fork disaster_monitor Failed"); exit(1); }
    else if (pid_B == 0) {
        execl("./disaster_monitor", "disaster_monitor", NULL);
        perror("[Error] disaster_monitor 실행 실패");
        exit(1);
    }
    printf("[Launcher] disaster_monitor (재난 감시) 가동됨 (PID: %d)\n", pid_B);

    printf("\n[Launcher] ✔ 모든 프로세스 가동 완료\n");
    printf("[Launcher] 감시 모드 진입...\n\n");

    // -------------------------------------------------------
    // 4. 자식 프로세스 감시 (Watchdog)
    // -------------------------------------------------------
    int status;
    pid_t child_pid;

    while ((child_pid = wait(&status)) > 0) {
        printf("\n[Launcher] ⚠️ 경고: 자식 프로세스(PID: %d)가 종료되었습니다.\n", child_pid);

        if (child_pid == pid_A) {
            printf(" >> vision_uploader가 중단됨! 점검 필요!\n");
            printf("    확인: sudo systemctl status libcamera\n");
        }
        else if (child_pid == pid_B) {
            printf(" >> disaster_monitor가 중단됨! 점검 필요!\n");
            printf("    확인: dmesg | tail -20\n");
        }
        else if (child_pid == pid_C) {
            printf(" >> main_controller가 중단됨! 점검 필요!\n");
            printf("    확인: ls -la /dev/ttyAMA*\n");
        }
    }

    return 0;
}
