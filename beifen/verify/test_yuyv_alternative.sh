#!/usr/bin/env bash
# 用 OpenCV 验证 YUYV 能不能替代 MJPG（每个最多 25 秒）
cd /home/rsve/Desktop/feibiao/verify || exit 1

echo "############ 编译 ocv_fmt ############"
g++ -std=c++17 -O2 -Wall ocv_fmt.cpp -o ocv_fmt $(pkg-config --cflags --libs opencv4) 2>/tmp/of.log \
  && echo "  ✓ 编译成功 error=0 warning=$(grep -c 'warning:' /tmp/of.log)" \
  || { echo "  ✗ 编译失败"; grep -n 'error:' /tmp/of.log | head; exit 1; }

echo
echo "############ 对照：工程当前配置 MJPG @120 ############"
timeout 25 ./ocv_fmt MJPG 120 30 2>/dev/null | grep -vE '^\[ WARN' | sed 's/^/  /'
echo "  rc=${PIPESTATUS[0]}"
sleep 4

echo
echo "############ 候选方案 A：YUYV @30 ############"
timeout 25 ./ocv_fmt YUYV 30 30 2>/dev/null | grep -vE '^\[ WARN' | sed 's/^/  /'
echo "  rc=${PIPESTATUS[0]}"
sleep 4

echo
echo "############ 候选方案 B：MJPG @30（看是不是帧率问题）############"
timeout 25 ./ocv_fmt MJPG 30 30 2>/dev/null | grep -vE '^\[ WARN' | sed 's/^/  /'
echo "  rc=${PIPESTATUS[0]}"

echo
echo "############ 残留进程 ############"
pgrep -a -f 'ocv_fmt|ffmpeg|uvc_' | grep -v pgrep | sed 's/^/  /' || echo "  无"
