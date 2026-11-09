#!/usr/bin/env bash
# 编译验证（不碰相机）
echo "############ 编译工程 Dart ############"
cd /home/rsve/Desktop/feibiao/Vision_Code/build || exit 1
make -j4 > /tmp/fix.log 2>&1
rc=$?
echo "  make rc=$rc   error=$(grep -c 'error:' /tmp/fix.log)   warning=$(grep -c 'warning:' /tmp/fix.log)"
grep -n 'error:'   /tmp/fix.log | head -20 | sed 's/^/    /'
grep -n 'warning:' /tmp/fix.log | head -20 | sed 's/^/    /'
ls -l Dart | sed 's/^/  /'

echo
echo "############ 确认改动到位 ############"
F=/home/rsve/Desktop/feibiao/Vision_Code/src/sensors/src/uvc/UvcCamera.cpp
echo "  --- 重连前是否退避（应有 500ms）---"
grep -n 'milliseconds(500)' "$F" | sed 's/^/    /'
echo "  --- 是否有重连次数上限 ---"
grep -n 'RECONNECT_LIMIT' "$F" | sed 's/^/    /'
echo "  --- 启动时是否校验出流 ---"
grep -n 'waitFirstFrame' "$F" | sed 's/^/    /'
echo "  --- 重连路径里是否还对曝光下手（应为 0 处）---"
awk '/第 2 级：连续失败太多/,/^    }$/' "$F" | grep -c 'applyExposureConfig' | sed 's/^/    applyExposureConfig 出现 /;s/$/ 次/'

echo
echo "############ 相机是否被碰过 ############"
pgrep -a -f 'uvc_|Dart|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无进程，编译过程没有打开相机"
