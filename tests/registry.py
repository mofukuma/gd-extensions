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
    for name in ("discord", "memcached", "supabase"):
        # gd package clientと同じくlibrary pathを安全な相対pathに限定する。
        manifest_text = (source / "extensions" / name / f"{name}.gdextension").read_text()
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
    subprocess.run(command + ["--version", "0.1.0"], check=True)
    # 二版目のsource設定を進め、既存版を壊さず追加できるか確かめる。
    for name in ("discord", "memcached", "supabase"):
        path = source / "extensions" / name / "gd.json"
        config = json.loads(path.read_text())
        config["version"] = "0.2.0"
        path.write_text(json.dumps(config))
    subprocess.run(command + ["--version", "0.2.0"], check=True)
    for name in ("discord", "memcached", "supabase"):
        meta = json.loads((site / "@mofukuma" / name / "meta.json").read_text())
        assert set(meta["versions"]) == {"0.1.0", "0.2.0"}
        assert meta["latest"] == "0.2.0"
        manifest = site / "@mofukuma" / name / "0.2.0" / f"{name}.gdextension"
        assert meta["versions"]["0.2.0"]["sha256"] == hashlib.sha256(manifest.read_bytes()).hexdigest()
        assert len(meta["versions"]["0.2.0"]["files"]) == 7
        # 同じ版のbinaryを差し替える不変版上書きを拒む。
        library = next(artifacts.glob(f"libgd{name}.*"))
        before = library.read_bytes()
        published = site / "@mofukuma" / name / "0.2.0" / "bin" / library.name
        published_before = published.read_bytes()
        library.write_bytes(before + b"changed")
        failed = subprocess.run(command + ["--version", "0.2.0"], capture_output=True).returncode
        assert failed != 0
        assert published.read_bytes() == published_before
        library.write_bytes(before)
    print("registry=3/3 versions=2 hashes=21/21")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
