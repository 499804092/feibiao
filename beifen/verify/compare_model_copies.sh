#!/usr/bin/env bash
# 对比所有 Katrin.bin 副本 + 定位测试工具
echo "=== 1. 虚拟机上所有 Katrin.bin / Katrin.xml 副本 ==="
find /home/rsve -name 'Katrin.*' 2>/dev/null | while read f; do
  sz=$(wc -c < "$f")
  md=$(md5sum "$f" | awk '{print $1}')
  printf "  %-72s %10s  %s\n" "$f" "$sz" "$md"
done

echo
echo "=== 2. 备份目录里的副本 ==="
ls -l /home/rsve/Desktop/feibiao/verify/../*.bak* 2>/dev/null | sed 's/^/  /'
find /home/rsve -maxdepth 4 -name '*Katrin*' -path '*bak*' 2>/dev/null | sed 's/^/  /'

echo
echo "=== 3. 测试工具真实位置 ==="
find /home/rsve/Desktop/feibiao -maxdepth 4 -type f -perm -u+x \( -name 'uvc_detect_view*' -o -name 'class_probe*' -o -name 'detect_logic_test*' -o -name 'Dart' \) 2>/dev/null | sed 's/^/  /'

echo
echo "=== 4. build 相关目录列表 ==="
ls -d /home/rsve/Desktop/feibiao/Vision_Code/*/ 2>/dev/null | sed 's/^/  /'
echo "  --- build_tools/ 内容 ---"
ls /home/rsve/Desktop/feibiao/Vision_Code/build_tools/ 2>/dev/null | head -30 | sed 's/^/    /'
