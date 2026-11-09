#!/usr/bin/env bash
# 对比不同 H 范围下的掩膜与轮廓（只抓一帧，不重复开相机）
FB=/home/rsve/Desktop/feibiao
cd "$FB/beifen/verify" || exit 1

g++ -std=c++17 -O2 -Wall diag_color.cpp \
    "$FB/Vision_Code/src/detect/src/color/ColorDetector.cpp" \
    -I "$FB/Vision_Code/src/detect/inc" \
    -o diag_color $(pkg-config --cflags --libs opencv4) 2>&1 | head -10

echo "========================= A) 当前阈值 H[40,80] ========================="
timeout 60 stdbuf -oL ./diag_color --width 800 --height 600 --fps 60 --orientation 2 \
  --config "$FB/Vision_Code/config/json/detect.json" 2>&1 \
  | sed -n '/2) 全图 HSV 统计/,/4) 所有轮廓/p' | head -30
echo "  --- 最大的几个轮廓（按面积）---"
timeout 60 stdbuf -oL ./diag_color --width 800 --height 600 --fps 60 --orientation 2 \
  --config "$FB/Vision_Code/config/json/detect.json" 2>&1 \
  | grep -E '^  轮廓|^     面积' | paste - - | sort -t= -k2 -rn | head -8

echo
echo "========================= B) 扩到 H[60,110] ========================="
timeout 60 stdbuf -oL ./diag_color --width 800 --height 600 --fps 60 --orientation 2 \
  --config "$FB/Vision_Code/config/json/detect.json" --h_min 60 --h_max 110 2>&1 \
  | sed -n '/3) 当前阈值/,/4) 所有轮廓/p' | head -8
echo "  --- 最大的几个轮廓（按面积）---"
timeout 60 stdbuf -oL ./diag_color --width 800 --height 600 --fps 60 --orientation 2 \
  --config "$FB/Vision_Code/config/json/detect.json" --h_min 60 --h_max 110 2>&1 \
  | grep -E '^  轮廓|^     面积' | paste - - | sort -t= -k2 -rn | head -8

echo
echo "========================= C) 更宽 H[35,120] + 降 S/V ========================="
timeout 60 stdbuf -oL ./diag_color --width 800 --height 600 --fps 60 --orientation 2 \
  --config "$FB/Vision_Code/config/json/detect.json" --h_min 35 --h_max 120 --s_min 40 --v_min 40 2>&1 \
  | sed -n '/3) 当前阈值/,/4) 所有轮廓/p' | head -8
echo "  --- 最大的几个轮廓（按面积）---"
timeout 60 stdbuf -oL ./diag_color --width 800 --height 600 --fps 60 --orientation 2 \
  --config "$FB/Vision_Code/config/json/detect.json" --h_min 35 --h_max 120 --s_min 40 --v_min 40 2>&1 \
  | grep -E '^  轮廓|^     面积' | paste - - | sort -t= -k2 -rn | head -8

echo
echo "===== 生成的图 ====="
ls -l frame.png mask.png 2>&1 | sed 's/^/  /'
