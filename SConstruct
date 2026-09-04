#!/usr/bin/env python
# 共通のgodot-cppを一度だけbuildし、公式拡張をまとめてlinkする。

import os

godot_cpp = os.path.abspath(ARGUMENTS.get("godot_cpp", "tmp/ref_godot_cpp"))  # binding sourceの場所
out = os.path.abspath(ARGUMENTS.get("out", "tmp"))  # 全生成物を置く場所
# 子SConstructがroot固有の引数を未知の設定として扱わないよう消す。
ARGUMENTS.pop("godot_cpp", None)
ARGUMENTS.pop("out", None)
SConsignFile(os.path.join(out, "extensions.sconsign.dblite"))  # build状態の保存先
env = SConscript(os.path.join(godot_cpp, "SConstruct"), {"api_version": "4.7"})  # 共通binding環境


# 一つの拡張に固有のsourceとincludeを共通bindingへlinkする。
def extension(name, includes, extra_sources=None):
    ext = env.Clone()
    ext.Append(CPPPATH=[os.path.join("extensions", name, "src")] + includes)
    sources = Glob(os.path.join("extensions", name, "src", "*.cpp"))
    sources += extra_sources or []
    target_dir = os.path.join(out, name + "_ext_bin")
    objects = []
    for source in sources:
        leaf = os.path.splitext(os.path.basename(str(source)))[0]
        objects.append(ext.SharedObject(os.path.join(target_dir, "obj", leaf), source))
    target = os.path.join(target_dir, "libgd{}{}{}".format(name, ext["suffix"], ext["SHLIBSUFFIX"]))
    library = ext.SharedLibrary(target, source=objects)
    ext.NoCache(library)
    return library


# 二つのnative配布libraryを同じGodot ABIとbuild条件で作る。
libraries = [
    extension("memcached", []),
    extension("supabase", []),
]
Default(libraries)
