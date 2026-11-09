#!/usr/bin/env bash
# 相机安全验证：先查占用，再列可调项（不取流），最后跑一次取流自检
echo "############ 0) 有没有残留进程占着相机 ############"
echo "--- /dev/video* 设备 ---"
ls -l /dev/video* 2>&1 | sed 's/^/  /'
echo "--- 占用 /dev/video0 的进程（空=没人占用，最好）---"
sudo -n fuser -v /dev/video0 2>/dev/null || fuser -v /dev/video0 2>/dev/null || echo "  (fuser 需要权限，改用 pgrep)"
echo "--- 相关进程 ---"
pgrep -a -f 'uvc_|Dart|v4l2-ctl|ffmpeg' | sed 's/^/  /' || echo "  无"

echo
echo "############ 1) 列出相机可调项（不取流，安全）############"
cd /home/rsve/Desktop/feibiao/verify || exit 1
timeout 40 ./uvc_detect_view --list-ctrls
echo "退出码 = $?"

echo
echo "############ 2) 取流自检：headless 跑 8 秒 ############"
echo "（这一步会真正开相机取流，跑完自动释放）"
export DISPLAY=:0
timeout 60 ./uvc_detect_view --orientation 2 --thr 0.20 --headless --seconds 8
echo "退出码 = $?"

echo
echo "############ 3) 相机是否已释放 ############"
sleep 1
pgrep -a -f 'uvc_detect_view' | sed 's/^/  仍在运行: /' || echo "  已释放，无残留进程"
