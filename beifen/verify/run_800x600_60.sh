#!/usr/bin/env bash
# ============================================================================
# 用 60fps 跑真实预览程序：看画面 + 验证曝光功能
# 安全设计：单次运行 / 看门狗自己退出 / 前后查进程
# ============================================================================
FB=/home/rsve/Desktop/feibiao
cd "$FB" || exit 1

echo "############ 0) 相机关闲检查 ############"
if pgrep -a -f 'uvc_detect_view|safe_stream_test|Dart|ffmpeg' | grep -v pgrep; then
  echo "  ⚠️ 有残留进程，中止"; exit 1
fi
echo "  ✅ 无残留进程"

echo
echo "############ 1) 当前配置（应只有 uvc 的分辨率/帧率/曝光被改）############"
echo "  --- uvc 节点 ---"
sed -n '/"uvc"/,/^  },/p' "$FB/Vision_Code/config/json/sensors.json" | sed 's/^/    /'
echo "  --- track.json 内参（应保持原值 1706.0 / 1705.1 / 320 / 320）---"
grep -A5 '"camera"' "$FB/Vision_Code/config/json/track.json" | sed 's/^/    /'

echo
echo "############ 2) 重新编译 uvc_detect_view（加了 --width/--height）############"
g++ -std=c++17 -O2 -Wall uvc_detect_view.cpp -o uvc_detect_view \
  $(pkg-config --cflags --libs opencv4) -lopenvino 2>/tmp/uv.log \
  && echo "  ✓ 编译成功 error=0 warning=$(grep -c 'warning:' /tmp/uv.log)" \
  || { echo "  ✗ 编译失败"; grep -n 'error:' /tmp/uv.log | head -20; exit 1; }

echo
echo "############ 3) 唯一的一次运行：800x600 @60 + 配置里的曝光 ############"
echo "  ---- 输出开始 ----"
timeout 70 ./uvc_detect_view --orientation 2 --thr 0.20 --width 800 --height 600 --fps 60 --headless --seconds 8 2>&1
rc=$?
echo "  ---- 输出结束（退出码 $rc）----"
case "$rc" in
  0)   echo "  ⇒ ✅ 画面正常 + 检测正常" ;;
  1)   echo "  ⇒ ❌ 出不了流（看门狗报错退出）" ;;
  124) echo "  ⇒ ⚠️ 被外层 timeout 兜住（异常）" ;;
  *)   echo "  ⇒ ⚠️ 退出码 $rc" ;;
esac

echo
echo "############ 4) 运行后：相机是否释放 ############"
sleep 1
if pgrep -a -f 'uvc_detect_view' | grep -v pgrep; then
  echo "  ⚠️ 有残留"
else
  echo "  ✅ 无残留，相机已释放"
fi
