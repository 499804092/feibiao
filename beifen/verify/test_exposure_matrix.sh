#!/usr/bin/env bash
# 决定性矩阵：是否「手动曝光模式」本身导致取流失败？
# 每种组合都先用 set_exposure 下发，再用 ocv_min 模式5（=工程当前做法）测出流
cd /home/rsve/Desktop/feibiao/verify || exit 1

try() {   # $1=说明  $2..=set_exposure 参数
  desc="$1"; shift
  echo "==========================================================="
  echo ">>> 组合: $desc"
  ./set_exposure "$@" 2>&1 | grep -E '^\s+(auto_exposure|exposure_time_absolute|exposure_auto_priority)\s+=' | sed 's/^/    /'
  sleep 2
  echo "    --- 用 OpenCV（模式5 = 工程的完整设置）测出流 ---"
  timeout 20 ./ocv_min 5 2>/dev/null | grep -vE '^\[ WARN' | head -2 | sed 's/^/    /'
  rc=${PIPESTATUS[0]}
  if [ "$rc" = "0" ]; then echo "    ⇒ ✅ 出流正常"
  elif [ "$rc" = "124" ]; then echo "    ⇒ ❌ 卡死（超时）"
  else echo "    ⇒ ❌ 失败 rc=$rc"; fi
  echo
  sleep 4
}

echo "############ 矩阵测试开始（每种最多 20 秒）############"
echo

try "① 自动曝光（出厂默认）" 0
try "② 手动 + 曝光157（=出厂曝光值）" 157 1
try "③ 手动 + 曝光40（你想要的值）+ priority=1" 40 1
try "④ 手动 + 曝光40 + priority=0" 40 0

echo "############ 收尾：恢复自动曝光 ############"
./set_exposure 0 2>&1 | tail -4 | sed 's/^/  /'
echo
echo "############ 残留进程 ############"
pgrep -a -f 'ocv_min|ffmpeg|uvc_' | grep -v pgrep | sed 's/^/  /' || echo "  无"
