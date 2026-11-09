#!/usr/bin/env bash
# 彻底恢复：USB 复位 → 曝光回出厂值 → 验证真能出流
cd /home/rsve/Desktop/feibiao/verify || exit 1
SUDO() { echo "$SUDOPW" | sudo -S -p '' "$@"; }
USBPATH=3-2

echo "############ 1) USB 软复位（清掉卡住的高带宽通道）############"
SUDO sh -c "echo 0 > /sys/bus/usb/devices/$USBPATH/authorized" && echo "  ✓ 取消授权"
sleep 2
SUDO sh -c "echo 1 > /sys/bus/usb/devices/$USBPATH/authorized" && echo "  ✓ 重新授权"
sleep 5
ls -l /dev/video0 2>&1 | sed 's/^/  /'

echo
echo "############ 2) 曝光恢复：先手动写回 157，再切回自动 ############"
echo "  （注意：自动模式下 exposure_time_absolute 是写不进去的，所以必须先切手动）"
./set_exposure 157 1 2>&1 | grep -E '✓|✗|-  ' | sed 's/^/    /'
sleep 1
./set_exposure 0 2>&1 | grep -E '✓|✗|-  ' | sed 's/^/    /'
sleep 1
for c in auto_exposure exposure_time_absolute; do
  printf '  %-28s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo
echo "############ 3) 验证：最小 OpenCV 取流 ############"
timeout 20 ./ocv_min 5 2>/dev/null | grep -vE '^\[ WARN' | sed 's/^/  /'
rc=${PIPESTATUS[0]}
[ "$rc" = "0" ] && echo "  ⇒ ✅ 能出流" || echo "  ⇒ ❌ 还是出不了流"

echo
echo "############ 4) 验证：真实的预览程序（工程完整链路，跑 6 秒）############"
export DISPLAY=:0
timeout 60 ./uvc_detect_view --no-cfg --orientation 2 --thr 0.20 --headless --seconds 6 2>&1 | tail -10 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
[ "$rc" = "0" ] && echo "  ⇒ ✅ 预览程序正常" || echo "  ⇒ ❌ 预览程序仍然失败 rc=$rc"

echo
echo "############ 5) 残留进程 ############"
pgrep -a -f 'uvc_|ocv_min|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无"
