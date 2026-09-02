#!/bin/bash
# 本家Godotで三つの拡張を読込み、公開APIと通信処理を実動検証する。
set -eu
cd "$(dirname "$0")/.."

GODOT=${GODOT:?Godot実行fileが要る} # 本家Godot 4.7.1の実行file
PLATFORM=${PLATFORM:-macos} # manifestが選ぶOS名
ARCH=${ARCH:-arm64} # manifestが選ぶCPU名
TARGET=${TARGET:-template_debug} # 起動するdebugまたはrelease library
LIMIT=${LIMIT:-90} # 一つの試験を待つ最大秒数

# platformに対応する共有library名を決める。
case "$PLATFORM" in
	macos) GODOT_TAG=macos; SUFFIX=dylib ;;
	linux) GODOT_TAG=linux; SUFFIX=so ;;
	*) echo "unsupported runtime platform: $PLATFORM" >&2; exit 2 ;;
esac

# 子processを期限内だけ動かす。
run_limited() {
	perl -e '$limit = shift; alarm $limit; exec @ARGV or exit 127' "$LIMIT" "$@"
}

# 一つの拡張を隔離した利用者領域で起動し、失敗時も診断結果を残す。
run_test() {
	local name=$1
	local checks=$2
	local project="tmp/${name}_ext_test"
	local user_dir="$PWD/tmp/${name}_ext_home"
	local output
	local code
	rm -rf "$user_dir"
	mkdir -p "$user_dir"
	set +e
	output=$(HOME="$user_dir" run_limited "$GODOT" --headless --path "$project" --script res://late_test.gd 2>&1)
	code=$?
	set -e
	printf '%s\n' "$output"
	test "$code" -eq 0
	grep -q "^checks=${checks} failures=0$" <<<"$output"
	! grep -Eq 'SCRIPT ERROR|^ERROR:' <<<"$output"
}

# manifestとlibraryを試験projectへ置く。
prepare() {
	local name=$1
	local project="tmp/${name}_ext_test"
	local library="tmp/${name}_ext_bin/libgd${name}.${GODOT_TAG}.${TARGET}.${ARCH}.${SUFFIX}"
	rm -rf "$project"
	mkdir -p "$project/bin" "$project/.godot"
	cp -R "tests/ext/$name/." "$project/"
	cp "extensions/$name/${name}.gdextension" "$project/"
	cp "$library" "$project/bin/"
	if [ -f "tests/ext/$name/extension_list.cfg" ]; then
		cp "tests/ext/$name/extension_list.cfg" "$project/.godot/"
	fi
}

# Discordは実行中load、Gateway、REST、unloadをまとめて確かめる。
prepare discord
mv tmp/discord_ext_test/.godot/extension_list.cfg tmp/discord_ext_test/extension_list.disabled
run_test discord 58

# Memcachedはlocal fake serverとの通信とunloadを確かめる。
prepare memcached
mv tmp/memcached_ext_test/.godot/extension_list.cfg tmp/memcached_ext_test/extension_list.disabled
run_test memcached 31

# Supabaseはlocal fake serverへDatabaseとAuthの要求を送る。
prepare supabase
mv tmp/supabase_ext_test/.godot/extension_list.cfg tmp/supabase_ext_test/extension_list.disabled
run_test supabase 21
