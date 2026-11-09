#!/usr/bin/env bash
# 只读式吞吐测量：三种带宽配置各测 5 秒，看真实帧率 + 抓驱动报错
D=/dev/video0
LOGBEFORE=/tmp/kern_before.txt
LOGAFTER=/tmp/kern_after.txt
tail -200 /var/log/kern.log > "$LOGBEFORE" 2>/dev/null

measure() {   # $1=说明 $2=格式 $3=分辨率
  desc="$1"; fmt="$2"; size="$3"
  echo "-------------------------------------------"
  echo ">>> $desc   ($fmt $size, 限时 5 秒)"
  out=/tmp/ff_$fmt_$size.log
  timeout 25 ffmpeg -hide_banner -nostdin -f v4l2 -input_format "$fmt" \
    -video_size "$size" -i "$D" -t 5 -f null - > "$out" 2>&1
  rc=$?
  # ffmpeg 的进度行里有 frame= 和 fps=
  line=$(grep -ao 'frame=[^ ]*.*fps=[^ ]*' "$out" | tail -1)
  if [ -n "$line" ]; then
    echo "    $line"
  else
    echo "    (拿不到帧率统计)"
  fi
  # 抓错误行
  errs=$(grep -aiE 'error|failed|cannot|invalid|no such|timeout|resource' "$out" | head -3)
  [ -n "$errs" ] && echo "$errs" | sed 's/^/    ⚠ /'
  if [ "$rc" = "124" ]; then echo "    ⇒ ❌ 卡死（超时）"
  elif [ "$rc" = "0" ]; then echo "    ⇒ ✅ ffmpeg 正常结束"
  else echo "    ⇒ ⚠️ rc=$rc"; fi
  sleep 3
}

echo "############ 三种带宽配置的实测吞吐 ############"
measure "配置1：工程当前用的高带宽" mjpeg 640x480
measure "配置2：同格式降到 1/4 像素"  mjpeg 320x240
measure "配置3：原始未压缩低带宽"     yuyv422 640x480

echo
echo "############ 期间内核有没有报错 ############"
tail -200 /var/log/kern.log > "$LOGAFTER" 2>/dev/null
diff "$LOGBEFORE" "$LOGAFTER" 2>/dev/null | grep -a '^>' | sed 's/^> //' | tail -20 | sed 's/^/  /' || echo "  内核日志无新增"
echo "  --- 最近 8 条相机相关日志 ---"
grep -a 'usb 3-2\|uvcvideo' /var/log/kern.log 2>/dev/null | tail -8 | sed 's/^/  /'

echo
echo "############ 残留进程 ############"
pgrep -a -f 'ffmpeg|uvc_|ocv_min' | grep -v pgrep | sed 's/^/  /' || echo "  无"
