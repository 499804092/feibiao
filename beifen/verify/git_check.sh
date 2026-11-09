#!/usr/bin/env bash
# 只读检查：上传 GitHub 需要的前置条件
FB=/home/rsve/Desktop/feibiao

echo "############ 1) git 有没有装 ############"
if command -v git >/dev/null; then
  echo "  ✅ git $(git --version | awk '{print $3}')"
else
  echo "  ❌ 没装 git（需要 sudo apt install git）"
fi

echo
echo "############ 2) git 身份有没有配 ############"
echo "  user.name  = $(git config --global user.name 2>/dev/null || echo '<未配置>')"
echo "  user.email = $(git config --global user.email 2>/dev/null || echo '<未配置>')"

echo
echo "############ 3) 有没有 SSH key（用于免密推送）############"
if [ -f ~/.ssh/id_ed25519.pub ]; then
  echo "  ✅ 有 ed25519 公钥:"; sed 's/^/     /' ~/.ssh/id_ed25519.pub
elif [ -f ~/.ssh/id_rsa.pub ]; then
  echo "  ✅ 有 rsa 公钥:"; sed 's/^/     /' ~/.ssh/id_rsa.pub
else
  echo "  ❌ 没有 SSH 公钥（~/.ssh/id_*.pub 不存在）"
fi

echo
echo "############ 4) 网络能不能通到 GitHub ############"
if timeout 10 git ls-remote https://github.com/499804092/feibiao.git >/dev/null 2>&1; then
  echo "  ✅ 能访问仓库（且可能是公开的，或已缓存凭证）"
elif timeout 10 curl -sI https://github.com >/dev/null 2>&1; then
  echo "  ⚠️ 能访问 github.com，但仓库需要认证（私有仓库或不存在）"
else
  echo "  ❌ 连不上 github.com（虚拟机可能没外网）"
fi

echo
echo "############ 5) 要上传的内容有多大 ############"
cd "$FB" || exit 1
echo "  各目录大小："
du -sh ./* 2>/dev/null | sed 's/^/    /'
echo "  总大小: $(du -sh . 2>/dev/null | cut -f1)"
echo "  大于 5MB 的文件（GitHub 单文件上限 100MB，超过 50MB 会警告）："
find . -type f -size +5M -exec ls -lh {} \; 2>/dev/null | awk '{printf "    %s  %s\n", $5, $9}'

echo
echo "############ 6) 是否已经是个 git 仓库 ############"
if [ -d "$FB/.git" ]; then
  echo "  ⚠️ 已经是 git 仓库"
  git -C "$FB" remote -v 2>/dev/null | sed 's/^/    /'
else
  echo "  ✅ 还不是 git 仓库（需要 git init）"
fi
