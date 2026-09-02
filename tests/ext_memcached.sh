#!/bin/bash
# Memcached GDExtensionをbuildし、localhostで実行中loadを検証する。
set -eu
cd "$(dirname "$0")/.."

GODOT=${GODOT:-/Applications/Godot\ 4.7.1.app/Contents/MacOS/Godot} # 本家Godot実行file
GD=${GD:-./bin/gd.macos.template_debug.arm64} # gd CLI実行file
PROJECT=tmp/memcached_ext_test # 検証projectの生成先
LIB=tmp/memcached_ext_bin/libgdmemcached.macos.template_debug.arm64.dylib # 検証するnative library

# 生成物をtmpへ揃えて本体sourceと混ぜない。
scons -C extensions/memcached godot_cpp=../../tmp/ref_godot_cpp out=../../tmp/memcached_ext_bin \
	platform=macos target=template_debug arch=arm64 -j12 >/dev/null
mkdir -p "$PROJECT/bin"
mkdir -p "$PROJECT/.godot"
cp tests/ext/memcached/project.godot tests/ext/memcached/fake_server.gd \
	tests/ext/memcached/late_test.gd tests/ext/memcached/typed_test.gd \
	tests/ext/memcached/packed_test.gd tests/ext/memcached/live_test.gd \
	tests/ext/memcached/startup_test.gd \
	extensions/memcached/memcached.gdextension "$PROJECT/"
cp tests/ext/memcached/extension_list.cfg "$PROJECT/.godot/"
cp "$LIB" "$PROJECT/bin/"

# extension一覧を外して、本家Godotで実行中load、通信、unloadを通す。
mv -f "$PROJECT/.godot/extension_list.cfg" "$PROJECT/extension_list.disabled"
set +e
LATE_OUT=$("$GODOT" --headless --path "$PROJECT" --script res://late_test.gd 2>&1)
LATE_CODE=$?
set -e
printf '%s\n' "$LATE_OUT"
test "$LATE_CODE" -eq 0
rg -q '^checks=31 failures=0$' <<<"$LATE_OUT"
! rg -q 'SCRIPT ERROR|^ERROR:' <<<"$LATE_OUT"

# 一覧から読む場合はgd固有の内部型とawait結果もstrictで推論する。
mv -f "$PROJECT/extension_list.disabled" "$PROJECT/.godot/extension_list.cfg"
GD_PATH=$(cd "$(dirname "$GD")" && pwd)/$(basename "$GD")
set +e
CHECK_OUT=$(cd "$PROJECT" && "$GD_PATH" --allow-ext --strict check typed_test.gd 2>&1)
CHECK_CODE=$?
set -e
test -z "$CHECK_OUT" || printf '%s\n' "$CHECK_OUT"
test "$CHECK_CODE" -eq 0
! rg -q 'SCRIPT ERROR|^ERROR:' <<<"$CHECK_OUT"

# 起動時に読んだextensionが通常終了時にも安全に片付くことを確かめる。
set +e
STARTUP_OUT=$("$GODOT" --headless --path "$PROJECT" --script res://startup_test.gd 2>&1)
STARTUP_CODE=$?
set -e
printf '%s\n' "$STARTUP_OUT"
test "$STARTUP_CODE" -eq 0
rg -q '^startup_memcached=true$' <<<"$STARTUP_OUT"
! rg -q 'SCRIPT ERROR|^ERROR:' <<<"$STARTUP_OUT"

# compile成果物にもmanifestとnative libraryを同梱し、単体で起動する。
(cd "$PROJECT" && "$GD_PATH" compile --output packed packed_test.gd >/dev/null)
PACKED_OUT=$(cd "$PROJECT" && ./packed 2>&1)
echo "$PACKED_OUT"
test "$PACKED_OUT" = "packed_memcached=true"
