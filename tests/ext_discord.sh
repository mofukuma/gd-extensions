#!/bin/bash
# Discord Bot GDExtensionをbuildし、localhostでGatewayとRESTを検証する。
set -eu
cd "$(dirname "$0")/.."

GODOT=${GODOT:-/Applications/Godot\ 4.7.1.app/Contents/MacOS/Godot} # 本家Godot実行file
GD=${GD:-./bin/gd.macos.template_debug.arm64} # gd CLI実行file
PROJECT=tmp/discord_ext_test # 検証projectの生成先
OTHER=tmp/discord_ext_other_test # project間共有を確かめる生成先
LIB=tmp/discord_ext_bin/libgddiscord.macos.template_debug.arm64.dylib # 検証するnative library
TEST_HOME="$PWD/tmp/discord_ext_home" # 本番の共有DBと分ける試験HOME
SIZE_HOME="$PWD/tmp/discord_ext_size_home" # 64 KiB page検証専用HOME
SIZE_BIG_HOME="$PWD/tmp/discord_ext_size_big_home" # 容量超過検証専用HOME
LIMIT=${LIMIT:-90} # 1processを待つ最大秒数

# 子processを期限内だけ動かす。
run_limited() {
	perl -e '$limit = shift; alarm $limit; exec @ARGV or exit 127' "$LIMIT" "$@"
}

# 生成物をtmpへ揃えて本体sourceと混ぜない。
scons -C extensions/discord godot_cpp=../../tmp/ref_godot_cpp out=../../tmp/discord_ext_bin \
	platform=macos target=template_debug arch=arm64 -j12 >/dev/null
mkdir -p "$PROJECT/bin"
mkdir -p "$PROJECT/.godot"
mkdir -p "$TEST_HOME"
mkdir -p "$SIZE_HOME"
mkdir -p "$SIZE_BIG_HOME"
rm -f "$TEST_HOME/.gd_cli_discord_limits.sqlite3" "$TEST_HOME/.gd_cli_discord_limits.sqlite3-shm" "$TEST_HOME/.gd_cli_discord_limits.sqlite3-wal"
rm -f "$SIZE_HOME/.gd_cli_discord_limits.sqlite3" "$SIZE_HOME/.gd_cli_discord_limits.sqlite3-shm" "$SIZE_HOME/.gd_cli_discord_limits.sqlite3-wal"
rm -f "$SIZE_BIG_HOME/.gd_cli_discord_limits.sqlite3" "$SIZE_BIG_HOME/.gd_cli_discord_limits.sqlite3-shm" "$SIZE_BIG_HOME/.gd_cli_discord_limits.sqlite3-wal"
cp tests/ext/discord/project.godot tests/ext/discord/fake_server.gd \
	tests/ext/discord/late_test.gd tests/ext/discord/persist_test.gd tests/ext/discord/typed_test.gd tests/ext/discord/packed_test.gd \
	tests/ext/discord/strict_test.gd \
	extensions/discord/discord.gdextension "$PROJECT/"
cp tests/ext/discord/extension_list.cfg "$PROJECT/.godot/"
cp "$LIB" "$PROJECT/bin/"

# 別projectにも同じextensionを置き、user dataが異なるprocessから永続上限を読む。
mkdir -p "$OTHER/bin" "$OTHER/.godot"
cp tests/ext/discord/project_other.godot "$OTHER/project.godot"
cp tests/ext/discord/fake_server.gd tests/ext/discord/persist_test.gd tests/ext/discord/rest_seed_test.gd \
	tests/ext/discord/rest_process_test.gd tests/ext/discord/rest_lock_test.gd extensions/discord/discord.gdextension "$OTHER/"
cp tests/ext/discord/size_test.gd "$OTHER/"
cp tests/ext/discord/size_reject_test.gd "$OTHER/"
cp tests/ext/discord/extension_list.cfg "$OTHER/.godot/"
cp "$LIB" "$OTHER/bin/"

# extension一覧を外し、本家Godotで実行中load、Gateway、REST、unloadを通す。
mv "$PROJECT/.godot/extension_list.cfg" "$PROJECT/extension_list.cfg"
set +e
LATE_OUT=$(run_limited env HOME="$TEST_HOME" "$GODOT" --headless --path "$PROJECT" --script res://late_test.gd 2>&1)
LATE_CODE=$?
set -e
printf '%s\n' "$LATE_OUT"
test "$LATE_CODE" -eq 0
rg -q '^checks=58 failures=0$' <<<"$LATE_OUT"
if rg -q 'SCRIPT ERROR|^ERROR:' <<<"$LATE_OUT"; then
	exit 1
fi

# 一覧から読む場合はgd固有の内部型とawait結果もstrictで推論する。
mv "$PROJECT/extension_list.cfg" "$PROJECT/.godot/extension_list.cfg"
GD_PATH=$(cd "$(dirname "$GD")" && pwd)/$(basename "$GD")
set +e
CHECK_OUT=$(cd "$PROJECT" && run_limited env HOME="$TEST_HOME" "$GD_PATH" --allow-ext --strict check typed_test.gd 2>&1)
CHECK_CODE=$?
set -e
test -z "$CHECK_OUT" || printf '%s\n' "$CHECK_OUT"
test "$CHECK_CODE" -eq 0
if rg -q 'SCRIPT ERROR|^ERROR:' <<<"$CHECK_OUT"; then
	exit 1
fi

# 別projectの別processを起動し、前processが消費したIdentify件数をSQLiteから引き継ぐ。
PERSIST_OUT=$(cd "$OTHER" && run_limited env HOME="$TEST_HOME" "$GD_PATH" --allow-ext run persist_test.gd 2>&1)
printf '%s\n' "$PERSIST_OUT"
rg -q '^persistent_discord=true$' <<<"$PERSIST_OUT"
if rg -q 'SCRIPT ERROR|^ERROR:' <<<"$PERSIST_OUT"; then
	exit 1
fi

# 前processのglobal 429をSQLiteで引き継ぎ、同tokenのRESTを停める。
REST_SEED_OUT=$(cd "$OTHER" && run_limited env HOME="$TEST_HOME" "$GD_PATH" --allow-ext run rest_seed_test.gd 2>&1)
printf '%s\n' "$REST_SEED_OUT"
rg -q '^rest_seed_discord=true$' <<<"$REST_SEED_OUT"
REST_PROCESS_OUT=$(cd "$OTHER" && run_limited env HOME="$TEST_HOME" "$GD_PATH" --allow-ext run rest_process_test.gd 2>&1)
printf '%s\n' "$REST_PROCESS_OUT"
rg -q '^rest_process_discord=true elapsed_ms=' <<<"$REST_PROCESS_OUT"
if rg -q 'SCRIPT ERROR|^ERROR:' <<<"$REST_SEED_OUT$REST_PROCESS_OUT"; then
	exit 1
fi

# 別processがwrite lockを保持してもmain threadを止めず送信しない。
DB="$TEST_HOME/.gd_cli_discord_limits.sqlite3"
(
	sqlite3 "$DB" <<'SQL'
BEGIN IMMEDIATE;
.shell sleep 1
ROLLBACK;
SQL
) &
LOCK_PID=$!
sleep 0.1
REST_LOCK_OUT=$(cd "$OTHER" && run_limited env HOME="$TEST_HOME" "$GD_PATH" --allow-ext run rest_lock_test.gd 2>&1)
wait "$LOCK_PID"
printf '%s\n' "$REST_LOCK_OUT"
rg -q '^rest_lock_discord=true frame_max_ms=' <<<"$REST_LOCK_OUT"
if rg -q 'SCRIPT ERROR|^ERROR:' <<<"$REST_LOCK_OUT"; then
	exit 1
fi

# 64 KiB pageの既存DBでも本体を64 MiBに制限する。
SIZE_DB="$SIZE_HOME/.gd_cli_discord_limits.sqlite3"
sqlite3 "$SIZE_DB" 'PRAGMA page_size=65536; VACUUM;'
SIZE_OUT=$(cd "$OTHER" && run_limited env HOME="$SIZE_HOME" "$GD_PATH" --allow-ext run size_test.gd 2>&1)
printf '%s\n' "$SIZE_OUT"
rg -q '^size_discord=true$' <<<"$SIZE_OUT"
test "$(sqlite3 "$SIZE_DB" 'PRAGMA page_size;')" = "65536"

# 64 MiBを越えた既存DBは開かず、外部送信もしない。
SIZE_BIG_DB="$SIZE_BIG_HOME/.gd_cli_discord_limits.sqlite3"
sqlite3 "$SIZE_BIG_DB" 'PRAGMA page_size=65536; CREATE TABLE padding(body BLOB); INSERT INTO padding VALUES(zeroblob(67108864));'
SIZE_BIG_OUT=$(cd "$OTHER" && run_limited env HOME="$SIZE_BIG_HOME" "$GD_PATH" --allow-ext run size_reject_test.gd 2>&1)
printf '%s\n' "$SIZE_BIG_OUT"
rg -q '^size_reject_discord=true$' <<<"$SIZE_BIG_OUT"

# localhostの名前許可だけでIP接続と待受の両方を通す。
STRICT_OUT=$(cd "$PROJECT" && run_limited env HOME="$TEST_HOME" "$GD_PATH" --allow-ext --strict --allow-net=localhost run strict_test.gd 2>&1)
printf '%s\n' "$STRICT_OUT"
rg -q '^strict_discord=true$' <<<"$STRICT_OUT"
if rg -q 'SCRIPT ERROR|^ERROR:' <<<"$STRICT_OUT"; then
	exit 1
fi

# compile成果物にもmanifestとnative libraryを同梱し、単体で起動する。
(cd "$PROJECT" && "$GD_PATH" compile --output packed packed_test.gd >/dev/null)
PACKED_OUT=$(cd "$PROJECT" && run_limited env HOME="$TEST_HOME" ./packed 2>&1)
printf '%s\n' "$PACKED_OUT"
test "$PACKED_OUT" = "packed_discord=true"

# 未送信で取消したinvalid予約を残さない。
test "$(sqlite3 "$DB" 'SELECT COUNT(*) FROM invalid_log WHERE invalid=0;')" = "0"
