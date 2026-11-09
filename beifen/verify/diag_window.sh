#!/usr/bin/env bash
# 诊断：窗口到底出现在哪个 display 上
cd "$(dirname "$0")" || exit 1

echo "=== 当前会话信息 ==="
echo "DISPLAY=$DISPLAY"
who
echo "X sockets:"
ls /tmp/.X11-unix/ 2>/dev/null

echo
echo "=== 后台启动 uvc_detect_view（DISPLAY=:0）==="
DISPLAY=:0 ./uvc_detect_view --orientation 2 --thr 0.20 > /tmp/dd.log 2>&1 &
PID=$!
sleep 8

if ps -p "$PID" > /dev/null 2>&1; then
    echo "进程仍在运行，PID=$PID  → 说明程序没崩，窗口应该已创建"
else
    echo "进程已退出 → 看下面日志找原因"
fi

echo
echo "=== 在 :0 上查找我们的窗口 ==="
DISPLAY=:0 xwininfo -root -children 2>/dev/null | grep -iE "uvc|detect" || echo "  :0 上没找到"

echo
echo "=== 在 :1 上查找我们的窗口 ==="
DISPLAY=:1 xwininfo -root -children 2>/dev/null | grep -iE "uvc|detect" || echo "  :1 上没找到"

echo
echo "=== 程序日志 ==="
cat /tmp/dd.log | head -12

echo
echo "=== 清理 ==="
kill "$PID" 2>/dev/null
sleep 2
pkill -f uvc_detect_view 2>/dev/null
echo "已退出"
