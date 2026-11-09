#!/usr/bin/env bash
# 用保存的真实相机帧，跑完整检测管线（不碰相机）
cd /home/rsve/Desktop/feibiao/verify || exit 1
export DISPLAY=:0
FRAME=/home/rsve/Desktop/feibiao/verify/uvc_frame.png

echo "=================== 1) uvc_detect_view --img ==================="
timeout 120 ./uvc_detect_view --img "$FRAME" --out /tmp/check_uvc.jpg --headless
echo "uvc_detect_view 退出码 = $?"

echo
echo "=================== 2) detect_logic_test ==================="
timeout 120 ./detect_logic_test "$FRAME"
echo "detect_logic_test 退出码 = $?"

echo
echo "=================== 3) 输出文件 ==================="
ls -l /tmp/check_uvc.jpg 2>&1
