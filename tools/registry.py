#!/usr/bin/env python3
"""release生成物からgdが検証できる静的拡張登録所を作る。"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path


PACKAGES = ("discord", "hello", "memcached", "supabase")  # 配布する公式package名。


# fileのsizeとSHA-256を登録所形式で返す。
def mark(path: Path) -> dict[str, object]:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"sha256": digest.hexdigest(), "size": path.stat().st_size}


# JSONをkey順の安定した形で保存する。
def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")


# 一つのpackage版を配置し、既存metaへ追加する。
def package(source: Path, artifacts: Path, site: Path, name: str, version: str) -> Path:
    src = source / "extensions" / name
    config = json.loads((src / "gd.json").read_text(encoding="utf-8"))
    if config.get("name") != f"@mofukuma/{name}" or config.get("version") != version:
        raise ValueError(f"{name}: gd.json and release version differ")
    target = site / "@mofukuma" / name
    main = src / config.get("main", "mod.gd")
    native = main.suffix == ".gdextension"
    if not main.is_file() or (not native and main.suffix != ".gd"):
        raise ValueError(f"{name}: .gd or .gdextension entry required")
    entry_name = main.name if native else "mod.gd"
    libraries: list[Path] = []
    if native:
        expected = {
            f"libgd{name}.macos.template_debug.arm64.dylib",
            f"libgd{name}.macos.template_release.arm64.dylib",
            f"libgd{name}.linux.template_debug.x86_64.so",
            f"libgd{name}.linux.template_release.x86_64.so",
            f"libgd{name}.windows.template_debug.x86_64.dll",
            f"libgd{name}.windows.template_release.x86_64.dll",
        }
        found = {item.name: item for item in artifacts.glob(f"**/libgd{name}.*") if item.name in expected}
        if set(found) != expected:
            raise ValueError(f"{name}: six runtime libraries required, got {len(found)}")
        libraries = [found[item] for item in sorted(expected)]
    files: dict[str, object] = {entry_name: mark(main)}
    for library in libraries:
        files[f"bin/{library.name}"] = mark(library)
    entry = {**mark(main), "files": files}
    meta_path = target / "meta.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {"versions": {}}
    old = meta["versions"].get(version)
    if old is not None and old != entry:
        raise ValueError(f"{name}@{version}: published version is immutable")

    # 不変版の照合後にだけ配布directoryを書き換える。
    release = target / version
    release.mkdir(parents=True, exist_ok=True)
    shutil.copy2(main, release / entry_name)
    if native:
        bin_dir = release / "bin"
        bin_dir.mkdir(exist_ok=True)
        for library in libraries:
            shutil.copy2(library, bin_dir / library.name)
    meta["description"] = config["description"]
    meta["versions"][version] = entry
    meta["latest"] = max(meta["versions"], key=lambda item: tuple(map(int, item.split("."))))
    write_json(meta_path, meta)
    shutil.copy2(src / "README.md", release / "README.md")
    shutil.copy2(source / "LICENSE.txt", release / "LICENSE.txt")
    shutil.copy2(source / "NOTICE.md", release / "NOTICE.md")
    return release


# 人が一覧を確認できる入口を作る。
def index(source: Path, site: Path) -> None:
    rows = []
    results = []
    for name in PACKAGES:
        meta = json.loads((site / "@mofukuma" / name / "meta.json").read_text(encoding="utf-8"))
        rows.append(f'<li><code>gd add ext:@mofukuma/{name}@^{meta["latest"]}</code> — {meta["description"]}</li>')
        results.append({"pkg": f"@mofukuma/{name}", "latest": meta["latest"], "description": meta["description"]})
    body = f"""<!doctype html>
<html lang="ja"><meta charset="utf-8"><title>gd extensions</title>
<h1>gd extensions</h1><ul>{"".join(rows)}</ul>
<p><a href="https://github.com/mofukuma/gd-extensions">source</a></p></html>
"""
    (site / "index.html").write_text(body, encoding="utf-8")
    (site / ".nojekyll").touch()
    write_json(site / "-" / "search", {"results": results})
    shutil.copy2(source / "LICENSE.txt", site / "LICENSE.txt")
    shutil.copy2(source / "NOTICE.md", site / "NOTICE.md")


# release tagと配置元を受け取り登録所を更新する。
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)", args.version):
        raise ValueError("version must be a canonical x.y.z value")
    for name in PACKAGES:
        package(args.source, args.artifacts, args.site, name, args.version)
    index(args.source, args.site)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
