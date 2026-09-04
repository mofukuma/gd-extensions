#!/usr/bin/env python3
"""静的登録所のpath、hash、版追加を小さいfixtureで検証する。"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


# fixture libraryを六platform分作る。
def libraries(root: Path, name: str) -> None:
    suffixes = (
        "macos.template_debug.arm64.dylib",
        "macos.template_release.arm64.dylib",
        "linux.template_debug.x86_64.so",
        "linux.template_release.x86_64.so",
        "windows.template_debug.x86_64.dll",
        "windows.template_release.x86_64.dll",
    )
    for suffix in suffixes:
        path = root / f"libgd{name}.{suffix}"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"{name}-{suffix}".encode())
    # Windows linkerの補助生成物は配布runtime libraryへ混ぜない。
    (root / f"libgd{name}.windows.template_debug.x86_64.lib").write_bytes(b"link helper")


# generatorを二版に対して動かし、過去版とhashを保つことを確かめる。
def main() -> int:
    root = Path("tmp/registry_test")
    artifacts = root / "artifacts"
    site = root / "site"
    source = root / "source"
    if root.exists():
        shutil.rmtree(root)
    shutil.copytree("extensions", source / "extensions")
    shutil.copy2("LICENSE.txt", source / "LICENSE.txt")
    shutil.copy2("NOTICE.md", source / "NOTICE.md")
    # include指定した純GDScriptの相対treeも配れることをfixtureで確かめる。
    (source / "extensions" / "discord" / "src" / "helper.gd").write_text("# fixture\n", encoding="utf-8")
    native = ("memcached", "supabase")
    packages = ("discord", "hello", *native)
    for name in native:
        # gd package clientと同じくlibrary pathを安全な相対pathに限定する。
        manifest_text = (source / "extensions" / name / f"{name}.gdextension").read_text(encoding="utf-8")
        library_section = manifest_text.split("[libraries]", 1)[1].split("\n[", 1)[0]
        paths = re.findall(r'^\S+\s*=\s*"([^"]+)"$', library_section, re.MULTILINE)
        assert len(paths) == 6
        assert all(path.startswith("bin/") and ":" not in path and ".." not in path.split("/") for path in paths)
        libraries(artifacts, name)
    command = [
        sys.executable,
        "tools/registry.py",
        "--source",
        str(source),
        "--artifacts",
        str(artifacts),
        "--site",
        str(site),
    ]
    # package tree内のlinkから外のfileを配らない。
    secret = source / "extensions" / "discord" / "secret.gd"
    leak = source / "extensions" / "discord" / "src" / "leak.gd"
    secret.write_text("# secret\n", encoding="utf-8")
    try:
        leak.symlink_to("../secret.gd")
    except OSError:
        pass
    else:
        failed = subprocess.run(command + ["--version", "0.1.2"], capture_output=True).returncode
        assert failed != 0
        leak.unlink()
    # native入口とartifactもlink経由で外を公開しない。
    native_main = source / "extensions" / "memcached" / "memcached.gdextension"
    native_real = native_main.with_suffix(".gdextension.real")
    native_main.rename(native_real)
    try:
        native_main.symlink_to(native_real.name)
    except OSError:
        native_real.rename(native_main)
    else:
        failed = subprocess.run(command + ["--version", "0.1.2"], capture_output=True).returncode
        assert failed != 0
        native_main.unlink()
        native_real.rename(native_main)
    artifact = artifacts / "libgdmemcached.macos.template_debug.arm64.dylib"
    artifact_real = artifact.with_suffix(".dylib.real")
    artifact.rename(artifact_real)
    try:
        artifact.symlink_to(artifact_real.name)
    except OSError:
        artifact_real.rename(artifact)
    else:
        failed = subprocess.run(command + ["--version", "0.1.2"], capture_output=True).returncode
        assert failed != 0
        artifact.unlink()
        artifact_real.rename(artifact)
    subprocess.run(command + ["--version", "0.1.2"], check=True)
    # 二版目のsource設定を進め、既存版を壊さず追加できるか確かめる。
    for name in packages:
        path = source / "extensions" / name / "gd.json"
        config = json.loads(path.read_text(encoding="utf-8"))
        config["version"] = "0.2.0"
        path.write_text(json.dumps(config), encoding="utf-8")
    subprocess.run(command + ["--version", "0.2.0"], check=True)
    catalog = json.loads((site / "-" / "catalog.json").read_text(encoding="utf-8"))
    assert {item["pkg"] for item in catalog["packages"]} == {
        "@mofukuma/discord", "@mofukuma/hello", "@mofukuma/memcached", "@mofukuma/supabase"
    }
    kinds = {item["pkg"]: item["kind"] for item in catalog["packages"]}
    assert kinds == {
        "@mofukuma/discord": "gd",
        "@mofukuma/hello": "gd",
        "@mofukuma/memcached": "ext",
        "@mofukuma/supabase": "ext",
    }
    # Pagesは純GDScript packageをnative拡張として案内しない。
    page = (site / "index.html").read_text(encoding="utf-8")
    assert "gd add hello gd:@mofukuma/hello@^0.2.0" in page
    assert "gd add discord gd:@mofukuma/discord@^0.2.0" in page
    assert "gd add ext:@mofukuma/hello" not in page
    assert "gd add ext:@mofukuma/discord" not in page
    assert not (site / "-" / "search").exists()
    for name in packages:
        meta = json.loads((site / "@mofukuma" / name / "meta.json").read_text(encoding="utf-8"))
        assert set(meta["versions"]) == {"0.1.2", "0.2.0"}
        assert meta["latest"] == "0.2.0"
        entry_name = f"{name}.gdextension" if name in native else "mod.gd"
        entry = site / "@mofukuma" / name / "0.2.0" / entry_name
        assert meta["versions"]["0.2.0"]["sha256"] == hashlib.sha256(entry.read_bytes()).hexdigest()
        expected = 7 if name in native else (2 if name == "discord" else 1)
        assert len(meta["versions"]["0.2.0"]["files"]) == expected
    assert (site / "@mofukuma" / "discord" / "0.2.0" / "helper.gd").read_text(encoding="utf-8") == "# fixture\n"
    for name in native:
        # 同じ版のbinaryを差し替える不変版上書きを拒む。
        meta = json.loads((site / "@mofukuma" / name / "meta.json").read_text(encoding="utf-8"))
        library = artifacts / f"libgd{name}.macos.template_debug.arm64.dylib"
        before = library.read_bytes()
        published = site / "@mofukuma" / name / "0.2.0" / "bin" / library.name
        published_before = published.read_bytes()
        library.write_bytes(before + b"changed")
        failed = subprocess.run(command + ["--version", "0.2.0"], capture_output=True).returncode
        assert failed != 0
        assert published.read_bytes() == published_before
        library.write_bytes(before)
    # 純GDScriptも元のfile名でなくmod.gdとして不変公開する。
    hello = source / "extensions" / "hello" / "src" / "mod.gd"
    published_hello = site / "@mofukuma" / "hello" / "0.2.0" / "mod.gd"
    hello_before = hello.read_bytes()
    published_before = published_hello.read_bytes()
    hello.write_bytes(hello_before + b"\n# changed\n")
    failed = subprocess.run(command + ["--version", "0.2.0"], capture_output=True).returncode
    assert failed != 0
    assert published_hello.read_bytes() == published_before
    print("registry=4/4 versions=2 hashes=21/21")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
