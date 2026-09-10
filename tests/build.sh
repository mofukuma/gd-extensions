#!/bin/bash
# 公式拡張を同じ条件でbuildし、生成物をtmpへ集める。
set -eu
cd "$(dirname "$0")/.."

PLATFORM=${1:?platformが要る} # Godotのbuild platform
ARCH=${2:?archが要る} # 配布binaryのCPU architecture
TARGET=${3:-template_debug} # debugまたはreleaseのbuild種別
JOBS=${JOBS:-2} # CI machineで同時に走らせるcompiler数
FLAGS=${FLAGS:-} # Windowsなどplatform固有のbuild設定
PROFILE=${PROFILE:-tools/build_profile.json} # 拡張が使うGodot APIだけを生成するprofile

# godot-cppを一度だけbuildし、二つのnative拡張へ共通にlinkする。
uvx --from scons==4.10.1 scons godot_cpp=tmp/ref_godot_cpp out=tmp \
	build_profile="$PROFILE" platform="$PLATFORM" arch="$ARCH" target="$TARGET" $FLAGS -j"$JOBS"

# godot-cppを別のbuild_profileで作り直すと、linkは通るのに読込み時へ欠落が残る。
# 健全なlibraryにgodot::の未解決記号は無いため、配る前にここで落とす。
# WindowsのDLLはnmが記号を出さないので対象から外す。
case "$PLATFORM" in
	linux) NM_FLAGS="-D --undefined-only" ;; # ELFの未解決記号は動的表から読む
	macos) NM_FLAGS="-u" ;;
	*) NM_FLAGS="" ;;
esac
if [ -n "$NM_FLAGS" ]; then
	for name in memcached supabase; do
		library=tmp/"$name"_ext_bin/libgd"$name"."$PLATFORM"."$TARGET"."$ARCH".* # 今回buildした共有library
		test -f $library
		missing=$(nm $NM_FLAGS $library | grep 'N5godot' || true)
		if [ -n "$missing" ]; then
			echo "undefined godot symbols in $library:" >&2
			printf '%s\n' "$missing" >&2
			echo "godot-cppを掃除して作り直すこと: git -C tmp/ref_godot_cpp clean -xfd" >&2
			exit 1
		fi
	done
fi
