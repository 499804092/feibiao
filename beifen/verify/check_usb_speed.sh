#!/usr/bin/env bash
# 只读：查相机在 USB 上协商到的速率（12M=全速 / 480M=高速）
echo "=== USB 拓扑 ==="
lsusb -t 2>/dev/null | sed 's/^/  /'

echo
echo "=== 相机节点速率 ==="
for d in /sys/bus/usb/devices/*/; do
  if [ -f "$d/idVendor" ]; then
    v=$(cat "$d/idVendor" 2>/dev/null)
    if [ "$v" = "0c45" ]; then
      echo "  路径: $d"
      echo "    idVendor/idProduct = $v/$(cat "$d/idProduct" 2>/dev/null)"
      echo "    speed   = $(cat "$d/speed" 2>/dev/null) Mbps"
      echo "    version = USB $(cat "$d/version" 2>/dev/null)"
      echo "    bMaxPower = $(cat "$d/bMaxPower" 2>/dev/null)"
    fi
  fi
done

echo
echo "=== 接口/端点信息（等时端点 = 视频流）==="
for d in /sys/bus/usb/devices/*/; do
  if [ -f "$d/idVendor" ] && [ "$(cat "$d/idVendor" 2>/dev/null)" = "0c45" ]; then
    base=$(basename "$d")
    for i in "$d"*/; do
      [ -f "$i/bInterfaceClass" ] || continue
      echo "  $(basename $i): class=$(cat $i/bInterfaceClass) subclass=$(cat $i/bInterfaceSubClass 2>/dev/null) driver=$(basename $(readlink -f $i/driver 2>/dev/null) 2>/dev/null)"
    done
    echo "  --- 该设备树下的 ep_* ---"
    find "$d" -maxdepth 3 -name 'ep_*' 2>/dev/null | head -10 | sed 's/^/    /'
  fi
done

echo
echo "=== 内核日志：相机最近枚举 ==="
grep -a 'usb 3-2\|uvcvideo' /var/log/kern.log 2>/dev/null | tail -10 | sed 's/^/  /'
