#!/usr/bin/env bash
# 640x480 @120 MJPG 安全测试（只测一次）
cd /home/rsve/Desktop/feibiao || exit 1

echo "===== 0) 相机在不在、有没有被占 ====="
if ! ls /dev/video0 >/dev/null 2>&1; then
  echo "  ❌ /dev/video0 不存在 —— 相机还没接回虚拟机"
  echo "     请在 VMware 里：虚拟机 → 可移动设备 → USB 2.0 Camera → 连接"
  exit 1
fi
echo "  ✅ /dev/video0 存在"
found=0
for p in /proc/[0-9]*; do
  if ls -l "$p/fd" 2>/dev/null | grep -q 'video0'; then
    echo "  ⚠️ PID ${p#/proc/} ($(cat $p/comm 2>/dev/null)) 占着相机，先清掉"
    pkill -TERM -f 'uvc_color_view|Dart'
    sleep 3
    found=1
  fi
done
[ "$found" = "0" ] && echo "  ✅ 没有进程占着相机"

echo
echo "===== 1) 当前配置 ====="
grep -E '"width"|"height"|"fps"|"mjpg"' Vision_Code/config/json/sensors.json | tail -4 | sed 's/^/  /'

echo
echo "===== 2) 只读：640x480 支持哪些格式 ====="
v4l2-ctl -d /dev/video0 --list-formats-ext 2>/dev/null | grep -B1 -A1 '640x480' | sed 's/^/  /'

echo
echo "===== 3) 单次安全测试：MJPG 640x480 @120（看门狗 12 秒，自己退出）====="
timeout 70 stdbuf -oL ./uvc_color_view --headless --seconds 6 2>&1 | grep -vE 'select\(\) timeout'
rc=${PIPESTATUS[0]}
case "$rc" in
  0) echo "  ⇒ ✅ 120fps 这档出流正常！" ;;
  1) echo "  ⇒ ❌ 这档还是读不到帧" ;;
  *) echo "  ⇒ ⚠️ 退出码 $rc" ;;
esac

echo
echo "===== 4) 测试后：残留检查 ====="
sleep 1
found=0
for p in /proc/[0-9]*; do
  if ls -l "$p/fd" 2>/dev/null | grep -q 'video0'; then
    echo "  ⚠️ 残留 PID ${p#/proc/}"; found=1
  fi
done
[ "$found" = "0" ] && echo "  ✅ 无残留，相机已释放"
