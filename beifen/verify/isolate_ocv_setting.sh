#!/usr/bin/env bash
# 逐个设置项隔离：找出 OpenCV 的哪一句 cap.set() 让流起不来
cd /home/rsve/Desktop/feibiao/verify || exit 1

echo "############ 0) 先恢复相机出厂默认 ############"
./set_exposure 0 2>&1 | grep -E 'auto_exposure|exposure_time_absolute|auto_priority' | tail -3 | sed 's/^/  /'
sleep 2

echo
echo "############ 1) 控制组：ffmpeg 直接读驱动（MJPG 640x480）############"
rm -f /tmp/c0.jpg
timeout 20 ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
  -video_size 640x480 -i /dev/video0 -frames:v 1 -update 1 -y /tmp/c0.jpg 2>&1 | tail -3 | sed 's/^/  /'
[ -s /tmp/c0.jpg ] && echo "  ✅ ffmpeg 成功：$(stat -c%s /tmp/c0.jpg) 字节" || echo "  ❌ ffmpeg 失败"
sleep 2

echo
echo "############ 2) 编译 ocv_min ############"
g++ -std=c++17 -O2 -Wall ocv_min.cpp -o ocv_min $(pkg-config --cflags --libs opencv4) 2>/tmp/om.log \
  && echo "  ✓ 编译成功" || { echo "  ✗ 编译失败"; grep -n 'error:' /tmp/om.log | head; exit 1; }

echo
echo "############ 3) 逐个隔离测试（每个最多 20 秒）############"
for m in 1 2 3 30 60 4 5; do
  timeout 20 ./ocv_min $m 2>/dev/null | grep -vE '^\[ WARN' | head -2 | sed 's/^/  /'
  rc=${PIPESTATUS[0]}
  if [ "$rc" = "124" ]; then echo "    → 结果: ❌ 卡死(超时)"; fi
  echo
  sleep 3      # 每次之间留时间让驱动清理
done

echo "############ 4) 残留进程 ############"
pgrep -a -f 'ocv_min|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无"
