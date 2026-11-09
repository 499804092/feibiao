#!/usr/bin/env bash
FB=/home/rsve/Desktop/feibiao
echo "############ feibiao 顶层（应只有 5 项）############"
ls -1 "$FB" | sed 's/^/  /'
echo
echo "############ beifen 结构 ############"
for d in 备份 旧文档 测试残留 verify; do
  printf '  beifen/%-10s %s 个文件\n' "$d" "$(ls -1 "$FB/beifen/$d" 2>/dev/null | wc -l)"
done
echo
echo "############ 工程运行必需的东西是否都在 ############"
for f in Vision_Code/main.cpp Vision_Code/CMakeLists.txt Vision_Code/build/Dart \
         Vision_Code/config/json/sensors.json Vision_Code/config/json/detect.json \
         Vision_Code/config/json/track.json \
         Vision_Code/config/openvino/Katrin.xml Vision_Code/config/openvino/Katrin.bin \
         uvc_detect_view uvc_detect_view.cpp 说明.md; do
  if [ -e "$FB/$f" ]; then printf '  ✅ %s\n' "$f"; else printf '  ❌ 缺失 %s\n' "$f"; fi
done
echo
echo "############ 相机参数（应为 120 / MJPG）############"
grep -n '"fps"\|"mjpg"' "$FB/Vision_Code/config/json/sensors.json" | sed 's/^/  /'
echo
echo "############ 模型文件校验（应匹配原始文件）############"
md5sum "$FB/Vision_Code/config/openvino/Katrin.xml" "$FB/Vision_Code/config/openvino/Katrin.bin" | sed 's/^/  /'
echo "  期望: 0bea800d936c7d66011dbd86fb241e0f (xml) / 58e4d1fdad3149c2851984134bfea81b (bin)"
echo
echo "############ 工程还能编译吗（只编译不运行）############"
cd "$FB/Vision_Code/build" && make -j4 > /tmp/fb.log 2>&1
echo "  make rc=$?  error=$(grep -c 'error:' /tmp/fb.log)  warning=$(grep -c 'warning:' /tmp/fb.log)"
echo
echo "############ 相机是否空闲 ############"
pgrep -a -f 'uvc_|Dart|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无进程占用，相机空闲"
