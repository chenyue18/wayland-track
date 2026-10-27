#!/usr/bin/env bash
# 安装 wayland-track 为 systemd 用户级服务（随图形会话自启，无需 root）
# 用法: ./install-service.sh [cpp|go]
set -euo pipefail

IMPL="${1:-cpp}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
UNIT_DIR="$HOME/.config/systemd/user"
PERSIST_DIR="$HOME/.local/share/wayland-track"

if [ "$IMPL" = "cpp" ]; then
    BIN="$REPO/cpp/build/wayland-track"
else
    BIN="$REPO/wayland-track"
fi
if [ ! -x "$BIN" ]; then
    echo "错误: 未找到可执行文件 $BIN（请先构建）" >&2
    exit 1
fi

mkdir -p "$UNIT_DIR" "$PERSIST_DIR/reports"
sed -e "s|%h/code/Angier_Research/wayland-track/cpp/build/wayland-track|$BIN|" \
    "$REPO/deploy/wayland-track.service" > "$UNIT_DIR/wayland-track.service"

# 停掉可能残留的手工 daemon（旧二进制）
if command -v "$BIN" > /dev/null; then
    "$BIN" restore > /dev/null 2>&1 || true
fi

systemctl --user daemon-reload
systemctl --user enable --now wayland-track.service
sleep 1
systemctl --user --no-pager status wayland-track.service | head -8
echo
echo "报告目录: $PERSIST_DIR/reports"
echo "日志:     journalctl --user -u wayland-track -f  或  $PERSIST_DIR/daemon.log"
echo "停止:     systemctl --user stop wayland-track（会自动恢复 socket 并落盘报告）"
echo "禁用自启: systemctl --user disable wayland-track"
