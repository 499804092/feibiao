#!/usr/bin/env bash
# 编译并运行颜色识别诊断（在 beifen/verify 目录下操作）
FB=/home/rsve/Desktop/feibiao
cd "$FB/beifen/verify" || exit 1

echo "===== 1) 相机关闲检查 ====="
f=0
for p in /proc/[0-9]*; do
  if ls -l "$p/fd" 2>/dev/null | grep -q video0; then
    echo "  ⚠️ PID ${p#/proc/} ($(cat $p/comm 2>/dev/null)) 占着相机"
    f=1
  fi
done
[ "$f" = "0" ] && echo "  ✅ 相机空闲"

echo
echo "===== 2) 编译诊断工具 ====="
g++ -std=c++17 -O2 -Wall diag_color.cpp \
    "$FB/Vision_Code/src/detect/src/color/ColorDetector.cpp" \
    -I "$FB/Vision_Code/src/detect/inc" \
    -o diag_color $(pkg-config --cflags --libs opencv4) 2>&1 | head -15
if [ -f diag_color ]; then
  echo "  ✅ 编译成功，$(wc -c < diag_color) 字节"
else
  echo "  ❌ 编译失败"; exit 1
fi

echo
echo "===== 3) 运行诊断（抓一帧分析）====="
timeout 60 stdbuf -oL ./diag_color \
    --width 800 --height 600 --fps 60 --orientation 2 \
    --config "$FB/Vision_Code/config/json/detect.json" 2>&1
echo "  退出码=$?"

echo
echo "===== 4) 生成的图片 ====="
ls -l frame.png mask.png 2>&1 | sed 's/^/  /'
