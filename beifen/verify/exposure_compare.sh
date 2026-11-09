#!/usr/bin/env bash
# ============================================================================
# exposure_compare.sh —— 曝光/降噪效果对比测试
# ----------------------------------------------------------------------------
# 依次设置不同的曝光值，每个值抓一帧，统计：
#   - 画面均值（亮度）
#   - 画面标准差（对比度/噪声）
#   - 对引导灯的检测结果
# 用来找出"噪声最小且检测仍正常"的曝光值。
#
# 用法： ./exposure_compare.sh
# ============================================================================
set -u
cd "$(dirname "$0")" || exit 1

DEV=/dev/video0
W=640; H=480

echo "================================================================"
echo " 曝光/降噪效果对比"
echo "================================================================"
echo
printf "%-14s %-10s %-10s %-30s\n" "曝光(行)" "画面均值" "标准差" "检测结果"
printf "%-14s %-10s %-10s %-30s\n" "--------" "--------" "------" "--------"

# 自动曝光作为基准
echo "--- 基准：自动曝光 ---"
v4l2-ctl -d $DEV --set-ctrl=auto_exposure=3 2>/dev/null
sleep 1
v4l2-ctl -d $DEV --set-ctrl=backlight_compensation=1 2>/dev/null
timeout 12 ./uvc_view --headless 0 $W $H 120 0 > /tmp/_auto.log 2>&1
grep -E "画面统计|检测" /tmp/_auto.log | head -2 | sed 's/^/  /'
echo

# 关闭自动曝光，逐个试手动值
v4l2-ctl -d $DEV --set-ctrl=auto_exposure=1 2>/dev/null
v4l2-ctl -d $DEV --set-ctrl=backlight_compensation=0 2>/dev/null
echo "--- 手动曝光 + 关背光补偿 ---"

for EXP in 10 20 40 60 80 120 157 250; do
    v4l2-ctl -d $DEV --set-ctrl=exposure_time_absolute=$EXP 2>/dev/null
    sleep 0.7

    # 抓 5 帧统计（避免第一帧曝光未稳定）
    v4l2-ctl -d $DEV --stream-mmap --stream-count=5 --stream-to=/dev/null >/dev/null 2>&1

    SHOT=/tmp/exp_${EXP}.jpg
    timeout 12 ./uvc_view --headless 0 $W $H 120 0 > /tmp/_e.log 2>&1
    MEAN=$(grep "画面统计" /tmp/_e.log | sed 's/.*均值 B=\([0-9.]*\).*/\1/' | head -1)
    STD=$(grep "画面统计" /tmp/_e.log | sed 's/.*标准差=\([0-9.]*\).*/\1/' | head -1)
    [ -z "$MEAN" ] && MEAN="?"
    [ -z "$STD" ] && STD="?"

    if [ -f view_shot.png ]; then
        cp view_shot.png "$SHOT" 2>/dev/null
    fi

    printf "%-14s %-10s %-10s %-30s\n" "$EXP" "$MEAN" "$STD" "见 /tmp/exp_${EXP}.jpg"
done

echo
echo "================================================================"
echo " 说明"
echo "================================================================"
echo "  画面均值 ↓ = 画面更暗（曝光更短）"
echo "  标准差   ↓ = 对比度/噪声更低（背景更干净）"
echo "  但均值太低（比如 <20）时，引导灯可能也一起变暗到认不出来"
echo
echo "  图片已存到 /tmp/exp_<曝光值>.jpg，可以拉回本机对比"
echo
echo "  当前仍未改变工程配置；确认合适的值后，改："
echo "    ~/Desktop/feibiao/Vision_Code/config/json/sensors.json → uvc.exposure_absolute"
