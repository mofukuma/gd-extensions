#!/bin/bash
# Supabase GDExtensionをbuildし、localhostで実行中loadを検証する。
set -eu
cd "$(dirname "$0")/.."

GODOT=${GODOT:-/Applications/Godot\ 4.7.1.app/Contents/MacOS/Godot}
GD=${GD:-./bin/gd.macos.template_debug.arm64}
PROJECT=tmp/supabase_ext_test
LIB=tmp/supabase_ext_bin/libgdsupabase.macos.template_debug.arm64.dylib

# 生成物をtmpへ揃えて本体sourceと混ぜない。
scons -C extensions/supabase godot_cpp=../../tmp/ref_godot_cpp out=../../tmp/supabase_ext_bin \
	platform=macos target=template_debug arch=arm64 -j12 >/dev/null
mkdir -p "$PROJECT/bin"
mkdir -p "$PROJECT/.godot"
cp tests/ext/supabase/project.godot tests/ext/supabase/fake_server.gd \
	tests/ext/supabase/late_test.gd tests/ext/supabase/typed_test.gd tests/ext/supabase/packed_test.gd \
	tests/ext/supabase/live_test.gd \
	extensions/supabase/supabase.gdextension "$PROJECT/"
cp tests/ext/supabase/extension_list.cfg "$PROJECT/.godot/"
cp "$LIB" "$PROJECT/bin/"

# extension一覧を外して、本家Godotで実行中load、通信、unloadを通す。
mv "$PROJECT/.godot/extension_list.cfg" "$PROJECT/extension_list.cfg"
"$GODOT" --headless --path "$PROJECT" --script res://late_test.gd

# 一覧から読む場合はgd固有の内部型とawait結果もstrictで推論する。
mv "$PROJECT/extension_list.cfg" "$PROJECT/.godot/extension_list.cfg"
GD_PATH=$(cd "$(dirname "$GD")" && pwd)/$(basename "$GD")
(cd "$PROJECT" && "$GD_PATH" --allow-ext --strict check typed_test.gd)

# compile成果物にもmanifestとnative libraryを同梱し、単体で起動する。
(cd "$PROJECT" && "$GD_PATH" compile --output packed packed_test.gd >/dev/null)
(cd "$PROJECT" && ./packed)
