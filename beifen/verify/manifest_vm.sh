#!/usr/bin/env bash
# 只读：输出需与本地一致的源文件/配置文件清单 (相对路径|大小|MD5)
cd /home/rsve/Desktop/feibiao || exit 1
FILES=$(mktemp)
for d in Vision_Code/config Vision_Code/src Electric_Control_Code; do
  [ -d "$d" ] && find "$d" -type f
done > "$FILES"
for f in Vision_Code/main.cpp Vision_Code/CMakeLists.txt; do
  [ -f "$f" ] && echo "$f" >> "$FILES"
done
find . -maxdepth 1 -type f -name '*.md' >> "$FILES" 2>/dev/null
sort -o "$FILES" "$FILES"
while IFS= read -r f; do
  printf '%s|%s|%s\n' "$f" "$(wc -c < "$f")" "$(md5sum "$f" | cut -d' ' -f1)"
done < "$FILES"
rm -f "$FILES"
