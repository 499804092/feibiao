#!/usr/bin/env bash
# 只读：抓刚才那次出流失败时内核/驱动报了什么错（不碰相机）
SUDO() { echo "$SUDOPW" | sudo -S -p '' "$@"; }

echo "############ 1) 完整 dmesg 里和相机/USB 相关的行 ############"
SUDO dmesg 2>/dev/null | grep -aiE 'uvc|video|ehci|xhci|usb 2-1|isoc|bandwidth|urb|epipe|reset' | tail -40 | sed 's/^/  /'
echo "  （如果这里没有报错，说明驱动层"看起来正常"，问题在更底层）"

echo
echo "############ 2) dmesg 最后 25 行（看失败时刻前后）############"
SUDO dmesg 2>/dev/null | tail -25 | sed 's/^/  /'

echo
echo "############ 3) UVC 驱动有没有记录带宽/接口设置问题 ############"
SUDO dmesg 2>/dev/null | grep -aiE 'Failed to set|bandwidth|no bandwidth|altsetting|alternative' | tail -15 | sed 's/^/  /' || echo "  无"

echo
echo "############ 4) 当前 USB 状态（只读 sysfs）############"
echo "  --- 相机在 2-1，看它的 power/状态 ---"
for f in speed version bMaxPower bConfigurationValue authorized; do
  printf '    %-24s = %s\n' "$f" "$(cat /sys/bus/usb/devices/2-1/$f 2>/dev/null)"
done
echo "  --- 这一条总线上还挂了什么（看有没有设备抢带宽）---"
lsusb -t 2>/dev/null | sed 's/^/    /'

echo
echo "############ 5) 相机是否空闲 ############"
pgrep -a -f 'safe_stream_test|uvc_detect_view' | grep -v pgrep | sed 's/^/  /' || echo "  无残留"
