#!/usr/bin/env bash
# 决定性实验：同样是「手动 + 40」，只差 auto_priority，看出流能不能起来
cd /home/rsve/Desktop/feibiao/verify || exit 1
export DISPLAY=:0

echo "############ 编译 set_exposure ############"
g++ -std=c++17 -O2 -Wall set_exposure.cpp -o set_exposure 2>/tmp/se.log \
  && echo "  ✓ 编译成功 0 error，warning $(grep -c 'warning:' /tmp/se.log)" \
  || { echo "  ✗ 编译失败"; grep -n 'error:' /tmp/se.log | head; exit 1; }

echo
echo "############ 实验 A：手动 40 + auto_priority=0（当前配置，预期卡死）############"
./set_exposure 40 0 2>&1 | grep -E '✓|-|auto_exposure|exposure_time_abs|auto_priority' | tail -6 | sed 's/^/  /'
echo "  --- 现在测出流（最多 30 秒）---"
timeout 30 ./uvc_detect_view --no-cfg --no-model --headless --seconds 4 2>&1 | grep -vE '^$' | tail -6 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
echo "  实验 A 结果: $([ "$rc" = "0" ] && echo '✅ 出流正常' || echo "❌ 卡死/失败 rc=$rc")"
sleep 3

echo
echo "############ 实验 B：手动 40 + auto_priority=1（只差这一项）############"
./set_exposure 40 1 2>&1 | grep -E '✓|-|auto_exposure|exposure_time_abs|auto_priority' | tail -6 | sed 's/^/  /'
echo "  --- 现在测出流（最多 30 秒）---"
timeout 30 ./uvc_detect_view --no-cfg --no-model --headless --seconds 4 2>&1 | grep -vE '^$' | tail -6 | sed 's/^/  /'
rc=${PIPESTATUS[0]}
echo "  实验 B 结果: $([ "$rc" = "0" ] && echo '✅ 出流正常' || echo "❌ 卡死/失败 rc=$rc")"

echo
echo "############ 残留进程 ############"
pgrep -a -f 'uvc_detect_view|ffmpeg' | grep -v pgrep | sed 's/^/  /' || echo "  无"
