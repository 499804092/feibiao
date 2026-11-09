#!/usr/bin/env bash
# 尝试软件复位 USB 相机（等效拔插），看能不能恢复 MJPG
# 密码从环境变量 SUDOPW 传入，不写进文件
D=/dev/video0
SUDO() { echo "$SUDOPW" | sudo -S -p '' "$@"; }

echo "############ 1) 先确认 YUYV 640x480 能不能出流 ############"
rm -f /tmp/t3.jpg
timeout 20 ffmpeg -hide_banner -loglevel warning -f v4l2 -input_format yuyv422 \
  -video_size 640x480 -i $D -frames:v 1 -update 1 -y /tmp/t3.jpg 2>&1 | tail -4 | sed 's/^/  /'
if [ -s /tmp/t3.jpg ]; then
  echo "  ✅ YUYV 640x480 成功：$(stat -c%s /tmp/t3.jpg) 字节"
else
  echo "  ❌ YUYV 640x480 也失败"
fi
sleep 2

echo
echo "############ 2) 找出相机挂在哪个 USB 端口 ############"
DEV=$(readlink -f /sys/class/video4linux/video0/device 2>/dev/null)
echo "  设备节点路径: $DEV"
USBPATH=$(echo "$DEV" | grep -oE '/[0-9]+-[0-9.]+(:[0-9.]+)?/' | head -1 | tr -d '/')
echo "  USB 端口号: $USBPATH"
if [ -z "$USBPATH" ]; then
  USBPATH=$(lsusb 2>/dev/null | grep -i '0c45:6368' | awk '{print $2"-"$4}' | tr -d ':')
  echo "  改用 lsusb 推断: $USBPATH"
fi

echo
echo "############ 3) 软件复位：取消授权 → 重新授权 ############"
if [ -n "$USBPATH" ] && [ -e "/sys/bus/usb/devices/$USBPATH/authorized" ]; then
  echo "  目标: /sys/bus/usb/devices/$USBPATH/authorized"
  echo "$SUDOPW" | sudo -S -p '' sh -c "echo 0 > /sys/bus/usb/devices/$USBPATH/authorized" && echo "  ✓ 已取消授权"
  sleep 2
  echo "$SUDOPW" | sudo -S -p '' sh -c "echo 1 > /sys/bus/usb/devices/$USBPATH/authorized" && echo "  ✓ 已重新授权"
  sleep 4
  echo "  --- 复位后内核日志 ---"
  tail -20 /var/log/kern.log 2>/dev/null | grep -iE 'usb|uvc|video' | tail -8 | sed 's/^/    /'
  echo "  --- 设备是否回来 ---"
  ls -l /dev/video* 2>&1 | sed 's/^/    /'
else
  echo "  ⚠️ 找不到 authorized 节点（USBPATH=$USBPATH），跳过软件复位"
fi

echo
echo "############ 4) 复位后再测 MJPG 640x480 ############"
rm -f /tmp/t4.jpg
timeout 20 ffmpeg -hide_banner -loglevel warning -f v4l2 -input_format mjpeg \
  -video_size 640x480 -i $D -frames:v 1 -update 1 -y /tmp/t4.jpg 2>&1 | tail -4 | sed 's/^/  /'
if [ -s /tmp/t4.jpg ]; then
  echo "  ✅ MJPG 640x480 恢复了！$(stat -c%s /tmp/t4.jpg) 字节"
else
  echo "  ❌ MJPG 640x480 仍然失败 —— 需要 VMware 里断开重连相机，或重启虚拟机"
fi

echo
echo "############ 5) 残留进程 ############"
pgrep -a -f 'ffmpeg|uvc_detect_view|v4l2-ctl' | grep -v pgrep | sed 's/^/  /' || echo "  无"
