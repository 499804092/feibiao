#!/usr/bin/env bash
# ============================================================================
# USB 兼容性改成 2.0 之后：确认生效 + 测 640x480@120 MJPG（只测一次）
# 安全设计：不写任何控制项 / 只开一次 / 不重试 / 看门狗自己退出
# ============================================================================
cd /home/rsve/Desktop/feibiao/beifen/verify || exit 1

echo "############ 0) 取流之前：确认相机空闲 ############"
if pgrep -a -f 'safe_stream_test|uvc_detect_view|Dart|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 有残留进程，中止"
  exit 1
fi
echo "  ✅ 无残留进程"

echo
echo "############ 1) 只读确认：USB 2.0 有没有生效 ############"
echo "  --- USB 拓扑（关键看相机挂在 xhci 还是 ehci 上）---"
lsusb -t 2>/dev/null | sed 's/^/    /'
echo "  --- 相机节点 ---"
for d in /sys/bus/usb/devices/*/; do
  [ -f "$d/idVendor" ] || continue
  [ "$(cat "$d/idVendor" 2>/dev/null)" = "0c45" ] || continue
  echo "    路径   : $d"
  echo "    速率   : $(cat $d/speed 2>/dev/null) Mbps"
  echo "    版本   : USB $(cat $d/version 2>/dev/null)"
done
echo "  --- 内核日志：最近一次枚举走的哪个控制器 ---"
tail -60 /var/log/kern.log 2>/dev/null | grep -a 'new .* USB device.*using' | tail -3 | sed 's/^/    /'
echo "  （若是 ehci-pci 而不是 xhci_hcd，说明 2.0 已生效）"

echo
echo "############ 2) 唯一的一次测试：MJPG 640x480 @120 ############"
echo "  ---- 输出开始 ----"
timeout 60 ./safe_stream_test 640 480 120 MJPG 30 15 2>&1
rc=$?
echo "  ---- 输出结束（退出码 $rc）----"
case "$rc" in
  0)   echo "  ⇒ ✅ 120fps 这一档能出流了！" ;;
  1)   echo "  ⇒ ❌ 还是出不了流" ;;
  124) echo "  ⇒ ⚠️ 看门狗没生效、被外层兜住（异常）" ;;
  *)   echo "  ⇒ ⚠️ 退出码 $rc" ;;
esac

echo
echo "############ 3) 取流之后：确认已释放 ############"
sleep 1
if pgrep -a -f 'safe_stream_test|uvc_detect_view' | grep -v pgrep; then
  echo "  ⚠️ 有残留"
else
  echo "  ✅ 无残留，相机已释放"
fi
