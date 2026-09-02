#!/bin/bash
# 静的登録所から実gdで追加し、本家Godotが配置後の拡張を起動できるか確かめる。
set -eu
cd "$(dirname "$0")/.."

GD=${GD:?gd実行fileが要る} # packageを取得する実gd
GODOT=${GODOT:?Godot実行fileが要る} # 配置後を起動する本家Godot
SITE=${SITE:?登録所directoryが要る} # HTTPで配信する生成済み登録所
VERSION=${VERSION:?検証する版が要る} # gd.jsonと登録所のpackage版
PORT=${PORT:-38925} # localhost試験serverの待受port
PROJECT=tmp/ecosystem_test # install結果を隔離する試験project
GD_PATH=$(cd "$(dirname "$GD")" && pwd)/$(basename "$GD") # project移動後も使える絶対path
SITE_PATH=$(cd "$SITE" && pwd) # HTTP serverへ渡す絶対path

# 終了時にlocalhostの静的serverを必ず止める。
cleanup() {
	kill "$SERVER_PID" 2>/dev/null || true
	wait "$SERVER_PID" 2>/dev/null || true
}

rm -rf "$PROJECT"
mkdir -p "$PROJECT"
printf '[application]\nconfig/name="extension ecosystem test"\n' > "$PROJECT/project.godot"
printf '{"name":"ecosystem-test","registry":"http://127.0.0.1:%s","imports":{}}\n' "$PORT" > "$PROJECT/gd.json"
cp tests/ext/startup_smoke.gd "$PROJECT/"
uv run --no-project python -m http.server "$PORT" --bind 127.0.0.1 --directory "$SITE_PATH" > tmp/ecosystem_http.log 2>&1 &
SERVER_PID=$!
trap cleanup EXIT INT TERM
sleep 1

# 静的な全件索引も実gdが指定語で絞り込む。
search_out=$(cd "$PROJECT" && "$GD_PATH" --allow-net=127.0.0.1:"$PORT" search discord)
grep -q '@mofukuma/discord' <<<"$search_out"
! grep -q '@mofukuma/memcached' <<<"$search_out"
no_match=$(cd "$PROJECT" && "$GD_PATH" --allow-net=127.0.0.1:"$PORT" search package-that-does-not-exist)
test "$no_match" = "no match"

# 三packageを同じprojectへ取り込み、manifestと現在platformのlibrary配置を確かめる。
for name in discord memcached supabase; do
	(cd "$PROJECT" && "$GD_PATH" --allow-net=127.0.0.1:"$PORT" add "ext:@mofukuma/$name@$VERSION")
	test -f "$PROJECT/vendor/ext/$name/$name.gdextension"
done
# 本家Godotに三Singletonを順に確認させる。
for singleton in GDDiscord GDMemcached GDSupabase; do
	GD_EXT_SINGLETON=$singleton "$GODOT" --headless --path "$PROJECT" --script res://startup_smoke.gd
done
