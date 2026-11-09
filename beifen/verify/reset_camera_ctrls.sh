#!/usr/bin/env bash
# 复位相机控制项到默认（清掉之前设过的曝光值）
DEV=/dev/video0

echo "=== 复位前 ==="
v4l2-ctl -d $DEV --list-ctrls 2>/dev/null | grep -iE "exposure|backlight"

echo
echo "=== 复位 ==="
v4l2-ctl -d $DEV --set-ctrl=auto_exposure=3           && echo "  ✓ auto_exposure=3 (自动)"
v4l2-ctl -d $DEV --set-ctrl=backlight_compensation=1  && echo "  ✓ backlight_compensation=1 (默认)"
v4l2-ctl -d $DEV --set-ctrl=brightness=0              && echo "  ✓ brightness=0"
v4l2-ctl -d $DEV --set-ctrl=gamma=100                 && echo "  ✓ gamma=100"

echo
echo "=== 复位后 ==="
v4l2-ctl -d $DEV --list-ctrls 2>/dev/null | grep -iE "exposure|backlight|brightness|gamma"

echo
echo "=== 测试能否出图 ==="
cd ~/Desktop/feibiao/verify || exit 1
timeout 15 ./uvc_view --headless 0 640 480 120 2 2>/dev/null | tail -5
