#!/usr/bin/env bash
# 只读：查看相机视频流接口的各个 alternate setting 及其端点带宽
# 目的：看 120fps 那一档是不是需要「高带宽等时端点」(wMaxPacketSize 带倍数)
SUDO() { echo "$SUDOPW" | sudo -S -p '' "$@"; }

echo "############ 视频流接口 (3-2:1.0 / 3-2:1.1) 的各 altsetting ############"
SUDO lsusb -v -d 0c45:6368 2>/dev/null | awk '
  /^      bInterfaceNumber|^      bAlternateSetting|^      bNumEndpoints|^        bmAttributes|^        wMaxPacketSize|^        bInterval|^      bInterfaceClass/ {
    gsub(/^ +/,""); print "  " $0
  }
' | head -60

echo
echo "############ 只看「等时(Isoc)」端点所在的行（bandwidth 关键）############"
SUDO lsusb -v -d 0c45:6368 2>/dev/null | grep -n -B4 -A2 'Transfer Type.*Isochronous' | head -60 | sed 's/^/  /'

echo
echo "############ 当前实际生效的 altsetting（只读）############"
for i in /sys/bus/usb/devices/3-2:1.0 /sys/bus/usb/devices/3-2:1.1; do
  [ -d "$i" ] || continue
  echo "  $(basename $i):"
  for f in "$i"/ep_*; do
    [ -e "$f" ] || continue
    n=$(basename "$f")
    echo "    $n: type=$(cat $f/type 2>/dev/null) maxpacket=$(cat $f/wMaxPacketSize 2>/dev/null) interval=$(cat $f/interval 2>/dev/null)"
  done
done
echo "  （没在取流时，等时端点通常不会出现在 sysfs 里，这是正常的）"
