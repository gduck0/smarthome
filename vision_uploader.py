#!/usr/bin/env python3

import time
from datetime import datetime
import lgpio
from picamera2 import Picamera2
import subprocess
import os

# =========================
# 핀 설정 (초음파는 A에서만 사용)
# =========================
TRIG = 15
ECHO = 18

DIST_THRESHOLD = 30   # cm
COOLDOWN = 5          # seconds
last_trigger = 0

# =========================
# 저장 / Drive 설정
# =========================
SAVE_DIR = "/home/pi/smart/12"
DRIVE_REMOTE = "gdrive:raspicam"
RCLONE_CONFIG = "/home/pi/.config/rclone/rclone.conf"

os.makedirs(SAVE_DIR, exist_ok=True)

# =========================
# GPIO init (lgpio)
# =========================
h = lgpio.gpiochip_open(0)
lgpio.gpio_claim_output(h, TRIG)
lgpio.gpio_claim_input(h, ECHO)
lgpio.gpio_write(h, TRIG, 0)
print("[vision_uploader] GPIO init OK (lgpio)", flush=True)

# =========================
# Camera init
# =========================
cam = Picamera2()
cam.start()
print("[vision_uploader] Camera ON", flush=True)

# =========================
# 초음파 거리 측정 함수
# =========================
def measure_distance():
    lgpio.gpio_write(h, TRIG, 0)
    time.sleep(0.000002)
    lgpio.gpio_write(h, TRIG, 1)
    time.sleep(0.00001)
    lgpio.gpio_write(h, TRIG, 0)

    start = time.time()
    while lgpio.gpio_read(h, ECHO) == 0:
        if time.time() - start > 0.03:
            return -1

    echo_start = time.time()
    while lgpio.gpio_read(h, ECHO) == 1:
        if time.time() - echo_start > 0.03:
            return -1

    echo_end = time.time()
    return int((echo_end - echo_start) * 34300 / 2)

# =========================
# 메인 루프
# =========================
print("[vision_uploader] Ultrasonic monitoring start", flush=True)

try:
    while True:
        dist = measure_distance()

        if 0 < dist <= DIST_THRESHOLD:
            now = time.time()

            if now - last_trigger >= COOLDOWN:
                filename = f"visitor_{datetime.now().strftime('%Y%m%d_%H%M%S')}.jpg"
                filepath = f"{SAVE_DIR}/{filename}"

                # 📸 사진 촬영
                cam.capture_file(filepath)
                print(
                    f"[vision_uploader] Capture saved: {filepath} (distance: {dist} cm)",
                    flush=True
                )

                # ☁️ Google Drive 업로드 (단일 파일)
                result = subprocess.run(
                    [
                        "/usr/bin/rclone",
                        "--config", RCLONE_CONFIG,
                        "copy",
                        filepath,
                        DRIVE_REMOTE,
                        "-v"
                    ],
                    capture_output=True,
                    text=True
                )

                if result.returncode == 0:
                    print("[vision_uploader] Uploaded to Google Drive", flush=True)
                else:
                    print("[vision_uploader] Upload FAILED", flush=True)
                    print(result.stderr, flush=True)

                last_trigger = now

        time.sleep(0.05)

except KeyboardInterrupt:
    print("[vision_uploader] Interrupted by user", flush=True)

finally:
    cam.stop()
    lgpio.gpiochip_close(h)
    print("[vision_uploader] Exit cleanly", flush=True)
