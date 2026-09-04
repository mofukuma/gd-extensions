#!/bin/bash
# 登録所から実gdで三拡張をinstallし、固定・cache・起動まで一続きで確かめる。
set -eu
cd "$(dirname "$0")/.."

GD=${GD:?gd実行fileが要る} # packageを取得する実gd
GODOT=${GODOT:-} # 配置後を任意で起動する本家Godot
SITE=${SITE:-} # localhostで配信する生成済み登録所
REGISTRY=${REGISTRY:-} # 公開済み登録所のURL
VERSION=${VERSION:?検証する版が要る} # gd.jsonと登録所のpackage版
PORT=${PORT:-38925} # localhost試験serverの待受port
PROJECT=tmp/ecosystem_test # install結果を隔離する試験project
CACHE="$PWD/tmp/ecosystem_cache" # offline installにも使う隔離cache
GD_PATH=$(cd "$(dirname "$GD")" && pwd)/$(basename "$GD") # project移動後も使える絶対path
SERVER_PID="" # localhost serverを起動した場合のprocess ID

# 終了時にlocalhostの静的serverを必ず止める。
cleanup() {
	if [ -n "$SERVER_PID" ]; then
		kill "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
}

rm -rf "$PROJECT" "$CACHE"
mkdir -p "$PROJECT" "$CACHE"
if [ -n "$SITE" ]; then
	SITE_PATH=$(cd "$SITE" && pwd) # HTTP serverへ渡す絶対path
	uv run --no-project python -m http.server "$PORT" --bind 127.0.0.1 --directory "$SITE_PATH" > tmp/ecosystem_http.log 2>&1 &
	SERVER_PID=$!
	REGISTRY="http://127.0.0.1:$PORT"
	for _n in $(seq 1 100); do
		curl -fsS "$REGISTRY/-/search" >/dev/null 2>&1 && break
		sleep 0.05
	done
fi
trap cleanup EXIT INT TERM
test -n "$REGISTRY"
curl -fsS --retry 30 --retry-delay 2 --retry-all-errors "$REGISTRY/-/search" >/dev/null

printf '[application]\nconfig/name="extension ecosystem test"\n' > "$PROJECT/project.godot"
printf '{"name":"ecosystem-test","registry":"%s","imports":{}}\n' "$REGISTRY" > "$PROJECT/gd.json"
cp tests/ext/startup_smoke.gd "$PROJECT/"
cp tests/ext/gd_smoke.gd "$PROJECT/"

# 静的な全件索引も実gdが指定語で絞り込む。
search_out=$(cd "$PROJECT" && GD_CACHE_HOME="$CACHE" "$GD_PATH" --allow-net --allow-env=GD_CACHE_HOME search discord)
grep -q '@mofukuma/discord' <<<"$search_out"
! grep -q '@mofukuma/memcached' <<<"$search_out"
no_match=$(cd "$PROJECT" && GD_CACHE_HOME="$CACHE" "$GD_PATH" --allow-net --allow-env=GD_CACHE_HOME search package-that-does-not-exist)
test "$no_match" = "no match"

# 純GDScript packageはnative拡張一覧を作らず、単一のpreload先だけを置く。
(cd "$PROJECT" && GD_CACHE_HOME="$CACHE" "$GD_PATH" --allow-net --allow-env=GD_CACHE_HOME add hello "gd:@mofukuma/hello@$VERSION")
test -f "$PROJECT/vendor/hello.gd"
test ! -e "$PROJECT/.godot/extension_list.cfg"

# 三native packageも同じprojectへ取り込み、manifest・lock・現在platformのlibrary配置を確かめる。
for name in discord memcached supabase; do
	(cd "$PROJECT" && GD_CACHE_HOME="$CACHE" "$GD_PATH" --allow-net --allow-env=GD_CACHE_HOME add "ext:@mofukuma/$name@$VERSION")
	test -f "$PROJECT/vendor/ext/$name/$name.gdextension"
done
test "$(grep -c '"@mofukuma/[^" ]*@' "$PROJECT/gd.lock")" = 4
platform_file=$(find "$PROJECT/vendor/ext" -type f \( -name '*.so' -o -name '*.dylib' -o -name '*.dll' \) | head -n 1)
test -n "$platform_file"

# frozen再解決でlockを変えず、networkなしのcache再配置も同一になることを確かめる。
cp "$PROJECT/gd.lock" "$PROJECT/gd.lock.before"
(cd "$PROJECT" && GD_CACHE_HOME="$CACHE" "$GD_PATH" --allow-net --allow-env=GD_CACHE_HOME install --frozen)
cmp "$PROJECT/gd.lock.before" "$PROJECT/gd.lock"
mv "$PROJECT/vendor" "$PROJECT/vendor.before"
mv "$PROJECT/.godot/extension_list.cfg" "$PROJECT/extension_list.before"
(cd "$PROJECT" && GD_CACHE_HOME="$CACHE" "$GD_PATH" --allow-env=GD_CACHE_HOME install --cached-only)
diff -ru "$PROJECT/vendor.before" "$PROJECT/vendor"
cmp "$PROJECT/extension_list.before" "$PROJECT/.godot/extension_list.cfg"
(cd "$PROJECT" && "$GD_PATH" --strict run gd_smoke.gd)
if [ -n "$GODOT" ]; then
	"$GODOT" --headless --path "$PROJECT" --script res://gd_smoke.gd
fi

# gdと、指定された場合は本家Godotにも三Singletonを順に確認させる。
for singleton in GDDiscord GDMemcached GDSupabase; do
	(cd "$PROJECT" && GD_EXT_SINGLETON=$singleton "$GD_PATH" --strict --allow-ext --allow-env=GD_EXT_SINGLETON run startup_smoke.gd)
	if [ -n "$GODOT" ]; then
		GD_EXT_SINGLETON=$singleton "$GODOT" --headless --path "$PROJECT" --script res://startup_smoke.gd
	fi
done
