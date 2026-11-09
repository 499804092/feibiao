#!/usr/bin/env bash
# 最后手段：重载 uvcvideo 驱动（会重新枚举相机）。不行就停手，改用 VMware 重连。
cd /home/rsve/Desktop/feibiao/verify || exit 1
SUDO() { echo "$SUDOPW" | sudo -S -p '' "$@"; }

echo "############ 1) 先确认没有进程占着相机 ############"
pgrep -a -f 'uvc_|ocv_min|ffmpeg|Dart' | grep -v pgrep | sed 's/^/  /' || echo "  无占用"

echo
echo "############ 2) uvcvideo 是不是模块 ############"
lsmod 2>/dev/null | grep -E 'uvcvideo|videodev' | sed 's/^/  /' || echo "  查不到"

echo
echo "############ 3) 卸载 uvcvideo ############"
SUDO modprobe -r uvcvideo 2>&1 | sed 's/^/  /'
echo "  卸载 rc=$?"
sleep 3
ls /dev/video* 2>&1 | sed 's/^/  卸载后设备: /'

echo
echo "############ 4) 重新加载 uvcvideo ############"
SUDO modprobe uvcvideo 2>&1 | sed 's/^/  /'
echo "  加载 rc=$?"
sleep 5
ls -l /dev/video* 2>&1 | sed 's/^/  /'
echo "  --- 内核日志 ---"
tail -12 /var/log/kern.log 2>/dev/null | grep -iE 'usb|uvc|video' | tail -6 | sed 's/^/    /'

echo
echo "############ 5) 测试能不能出流（只测一次，不反复戳）############"
timeout 20 ./ocv_min 5 2>/dev/null | grep -vE '^\[ WARN' | sed 's/^/  /'
rc=${PIPESTATUS[0]}
if [ "$rc" = "0" ]; then
  echo "  ⇒ ✅ 恢复了！"
else
  echo "  ⇒ ❌ 还是不行 —— 到这里就停手，需要你在 VMware 里操作："
  echo "       虚拟机 → 可移动设备 → USB 2.0 Camera → 断开连接，等 3 秒再连接"
  echo "       （或者直接重启虚拟机，最彻底）"
fi

echo
echo "############ 6) 残留进程 ############"
pgrep -a -f 'uvc_|ocv_min|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无"
