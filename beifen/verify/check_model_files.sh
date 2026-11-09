#!/usr/bin/env bash
# 只读检查：模型文件完整性 + ScatterND 节点来源 + 可用测试图片
FB=/home/rsve/Desktop/feibiao
MDL=$(find "$FB" -name 'Katrin.bin' 2>/dev/null | head -1)
MDLDIR=$(dirname "$MDL")

echo "=== 1. 模型文件路径与大小 ==="
ls -l "$MDLDIR"/Katrin.xml "$MDLDIR"/Katrin.bin 2>&1 | sed 's/^/  /'

echo
echo "=== 2. MD5 校验（应为 0bea800d936c7d66011dbd86fb241e0f / 58e4d1fdad3149c2851984134bfea81b）==="
md5sum "$MDLDIR"/Katrin.xml "$MDLDIR"/Katrin.bin 2>&1 | sed 's/^/  /'

echo
echo "=== 3. 是否含 NUL 字节（全 0 文件检查）==="
for f in "$MDLDIR"/Katrin.xml "$MDLDIR"/Katrin.bin; do
  n=$(tr -d '\0' < "$f" | wc -c)
  t=$(wc -c < "$f")
  echo "  $(basename $f): 总字节=$t  非NUL字节=$n"
done

echo
echo "=== 4. XML 里 ScatterND 出现次数 ==="
grep -o 'ScatterNDUpdate' "$MDLDIR"/Katrin.xml 2>/dev/null | wc -l | sed 's/^/  出现 /;s/$/ 次/'

echo
echo "=== 5. ScatterND 节点上下文（看 indices 是不是 Constant）==="
grep -n 'ScatterND' "$MDLDIR"/Katrin.xml | head -20 | sed 's/^/  /'

echo
echo "=== 6. 可用测试图片 ==="
find "$FB" -maxdepth 3 \( -iname '*.jpg' -o -iname '*.png' -o -iname '*.jpeg' -o -iname '*.bmp' \) 2>/dev/null | head -30 | sed 's/^/  /'

echo
echo "=== 7. 已编译的测试工具 ==="
ls -l "$FB"/Vision_Code/build/class_probe "$FB"/Vision_Code/build/detect_logic_test "$FB"/Vision_Code/build/uvc_detect_view 2>&1 | sed 's/^/  /'
