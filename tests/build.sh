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

# godot-cppを一度だけbuildし、三つの拡張へ共通にlinkする。
uvx --from scons==4.10.1 scons godot_cpp=tmp/ref_godot_cpp out=tmp \
	build_profile="$PROFILE" platform="$PLATFORM" arch="$ARCH" target="$TARGET" $FLAGS -j"$JOBS"
