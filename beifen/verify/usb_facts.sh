#!/usr/bin/env bash
# 只读：把「相机是什么 USB 设备」和「它挂在哪类控制器上」分清
echo "=========== 1) USB 控制器清单（lspci）==========="
lspci -nn 2>/dev/null | grep -i 'usb\|xhci\|ehci\|uhci' | sed 's/^/  /'

echo
echo "=========== 2) 内核认到的 USB 总线 ==========="
lsusb -t 2>/dev/null | sed 's/^/  /'

echo
echo "=========== 3) 相机设备描述符（只读 sysfs）==========="
for d in /sys/bus/usb/devices/*/; do
  [ -f "$d/idVendor" ] || continue
  [ "$(cat "$d/idVendor" 2>/dev/null)" = "0c45" ] || continue
  echo "  路径            : $d"
  echo "  idVendor:idProduct : $(cat $d/idVendor):$(cat $d/idProduct)"
  echo "  bcdUSB (version)   : $(cat $d/version 2>/dev/null)      <- 设备自己声明的 USB 规范版本"
  echo "  bcdDevice          : $(cat $d/bcdDevice 2>/dev/null)"
  echo "  speed              : $(cat $d/speed 2>/dev/null) Mbps   <- 实际协商速率"
  echo "  bMaxPower          : $(cat $d/bMaxPower 2>/dev/null)"
  echo "  manufacturer       : $(cat $d/manufacturer 2>/dev/null)"
  echo "  product            : $(cat $d/product 2>/dev/null)"
done

echo
echo "=========== 4) 相机所在总线的根集线器驱动 ==========="
for d in /sys/bus/usb/devices/usb*/; do
  echo "  $(basename $d): driver=$(basename $(readlink -f $d/driver 2>/dev/null))  speed=$(cat $d/speed 2>/dev/null)  product=$(cat $d/product 2>/dev/null)"
done

echo
echo "=========== 5) 相机所在总线上还挂了什么 ==========="
echo "  （看有没有别的设备在抢带宽）"
lsusb 2>/dev/null | sed 's/^/  /'

echo
echo "=========== 6) 视频流接口的端点信息 ==========="
for e in /sys/bus/usb/devices/3-2*/ep_*; do
  [ -e "$e" ] || continue
  echo "  $(basename $e): type=$(cat $e/type 2>/dev/null) maxpacket=$(cat $e/wMaxPacketSize 2>/dev/null) interval=$(cat $e/interval 2>/dev/null)"
done
