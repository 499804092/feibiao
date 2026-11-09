#!/usr/bin/env bash
# ============================================================================
# 安全测试：相机能不能出流 + HSV 识别能不能跑起来
# ----------------------------------------------------------------------------
# 安全设计：
#   ① uvc_color_view 全程【不写任何 V4L2 控制项】（只设 FOURCC/宽/高/帧率这些取流参数）
#   ② 超时靠程序【自己的看门狗】干净退出，不用外部 kill
#   ③ 最多测两次（120fps 一次、60fps 一次），每次都是干净退出，不重试不循环
# ============================================================================
FB=/home/rsve/Desktop/feibiao
cd "$FB" || exit 1

echo "############ 0) 相机有没有被别人占着（最关键）############"
if pgrep -a -f 'uvc_color_view|uvc_detect_view|Dart|safe_stream_test|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 上面有残留进程！先清掉再测，否则必然失败"
  pkill -TERM -f 'uvc_color_view|uvc_detect_view|safe_stream_test'
  sleep 3
  pgrep -a -f 'uvc_color_view|uvc_detect_view|safe_stream_test' | grep -v pgrep || echo "  已清理干净"
else
  echo "  ✅ 无残留进程，相机空闲"
fi

echo
echo "############ 1) 只读：相机在不在 + 挂哪个控制器 ############"
ls -l /dev/video0 2>&1 | sed 's/^/  /'
echo "  --- USB 拓扑 ---"
lsusb -t 2>/dev/null | grep -E 'root_hub|Video' | sed 's/^/    /'
echo "  --- 640x480 支持的格式 ---"
v4l2-ctl -d /dev/video0 --list-formats-ext 2>/dev/null | grep -B1 -A1 '640x480' | sed 's/^/    /'
echo "  --- 当前曝光（只读）---"
for c in auto_exposure exposure_time_absolute; do
  printf '    %-26s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo
echo "############ 2) 安全测试 ①：640x480 @120 ############"
echo "  ---- 输出开始 ----"
timeout 70 ./uvc_color_view --headless --seconds 8 2>&1 | grep -vE 'select\(\) timeout'
rc=${PIPESTATUS[0]}
echo "  ---- 输出结束（退出码 $rc）----"
if [ "$rc" = "0" ]; then
  echo "  ⇒ ✅ 120fps 这一档出流正常"
  OK=1
else
  echo "  ⇒ ❌ 120fps 这一档没出流"
  OK=0
fi

if [ "$OK" = "0" ]; then
  echo
  echo "############ 3) 安全测试 ②：640x480 @120 不行，换 800x600 @60 ############"
  sleep 3
  echo "  ---- 输出开始 ----"
  timeout 70 ./uvc_color_view --headless --seconds 8 --width 800 --height 600 --fps 60 2>&1 | grep -vE 'select\(\) timeout'
  rc=${PIPESTATUS[0]}
  echo "  ---- 输出结束（退出码 $rc）----"
  [ "$rc" = "0" ] && echo "  ⇒ ✅ 60fps 这一档可用" || echo "  ⇒ ❌ 60fps 也不行"
fi

echo
echo "############ 4) 测试后：相机是否已释放 ############"
sleep 1
pgrep -a -f 'uvc_color_view|safe_stream_test' | grep -v pgrep | sed 's/^/  ⚠️ 残留: /' || echo "  ✅ 无残留，相机已释放"
