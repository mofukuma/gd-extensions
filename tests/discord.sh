#!/bin/bash
# Discord純GDScript packageを利用者layoutへ置き、localhost通信を検証する。
set -eu
cd "$(dirname "$0")/.."

GODOT=${GODOT:?Godot実行fileが要る} # 本家Godotの実行file
LIMIT=${LIMIT:-90} # 試験を待つ最大秒数
PROJECT=tmp/discord_gd_test # 利用者projectの生成先

# 子processを期限内だけ動かす。
run_limited() {
	perl -e '$limit = shift; alarm $limit; exec @ARGV or exit 127' "$LIMIT" "$@"
}

rm -rf "$PROJECT"
mkdir -p "$PROJECT/discord"
cp tests/ext/discord/project.godot "$PROJECT/project.godot"
cp tests/ext/discord/fake_server.gd tests/ext/discord/late_test.gd "$PROJECT/"
cp extensions/discord/src/mod.gd "$PROJECT/discord/mod.gd"

set +e
output=$(run_limited "$GODOT" --headless --path "$PROJECT" --script res://late_test.gd 2>&1)
code=$?
set -e
printf '%s\n' "$output"
test "$code" -eq 0
grep -q '^checks=25 failures=0$' <<<"$output"
! grep -Eq 'SCRIPT ERROR|^ERROR:' <<<"$output"
