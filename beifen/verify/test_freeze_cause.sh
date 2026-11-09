#!/usr/bin/env bash
# 对照实验：自动曝光 vs 手动40，看哪个会卡死
# 每个测试都套 timeout，卡住会自动被杀，不会留死进程
cd /home/rsve/Desktop/feibiao/verify || exit 1
export DISPLAY=:0

probe() {   # $1 = 说明文字  其余 = 传给 uvc_detect_view 的参数
  desc="$1"; shift
  echo "-------------------------------------------"
  echo ">>> $desc"
  timeout -k 5 30 ./uvc_detect_view "$@" 2>&1 | tail -8
  rc=${PIPESTATUS[0]}
  if [ "$rc" = "124" ] || [ "$rc" = "137" ]; then
    echo "  ❌ 结果：卡死（超时被杀，rc=$rc）"
  elif [ "$rc" = "0" ]; then
    echo "  ✅ 结果：正常出流（rc=0）"
  else
    echo "  ⚠️ 结果：异常退出 rc=$rc"
  fi
  echo
  sleep 2
}

echo "############ 0) 先恢复相机默认（自动曝光）############"
./set_exposure 0 2>&1 | tail -5

echo
echo "############ 基线测试：自动曝光（--no-cfg，不读配置）############"
probe "自动曝光 + 只看画面，5 秒" --no-cfg --no-model --headless --seconds 5

echo "############ 实验组：按配置下发「手动 40」再取流 ############"
probe "读配置(手动40) + 只看画面，5 秒" --no-model --headless --seconds 5

echo "############ 检查有没有留下卡死的进程 ############"
pgrep -a -f 'uvc_detect_view' | grep -v pgrep | sed 's/^/  仍在运行: /' || echo "  无残留进程"
