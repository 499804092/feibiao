#!/usr/bin/env bash
# 二分找出「能出流的最低曝光值」——你要的越暗越好，这就是边界
cd /home/rsve/Desktop/feibiao/verify || exit 1

test_val() {   # $1 = 曝光值，返回 0=能出流
  ./set_exposure "$1" 1 >/dev/null 2>&1
  sleep 1
  timeout 20 ./ocv_min 5 >/dev/null 2>&1
  return $?
}

echo "############ 扫描能出流的曝光下限（每种最多 20 秒）############"
echo "  （手动模式，priority=1；157 已知可用，40 已知不可用）"
echo
for v in 60 80 100 120 140; do
  if test_val $v; then
    echo "  曝光 $v  ⇒ ✅ 能出流"
  else
    echo "  曝光 $v  ⇒ ❌ 出不了流"
  fi
  sleep 3
done

echo
echo "############ 恢复自动曝光 ############"
./set_exposure 0 >/dev/null 2>&1
for c in auto_exposure exposure_time_absolute; do
  printf '  %-28s = %s\n' "$c" "$(v4l2-ctl -d /dev/video0 --get-ctrl=$c 2>/dev/null | sed 's/.*: //')"
done

echo
echo "############ 残留进程 ############"
pgrep -a -f 'ocv_min|uvc_' | grep -v pgrep | sed 's/^/  /' || echo "  无"
