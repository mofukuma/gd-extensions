# gd extensions

gd と Godot 4.7 から使える公式 GDExtension 集です。公開 API は `GD` から始まる短い名前に揃えています。

## 使う

gd 0.1.0 以降で、project の中から必要な拡張だけ追加します。

```sh
gd add ext:@mofukuma/discord@^0.1.0
gd add ext:@mofukuma/memcached@^0.1.0
gd add ext:@mofukuma/supabase@^0.1.0
```

`gd install` は現在の OS に合う native library と manifest の SHA-256 を検証し、`vendor/ext/`へ置きます。

| package | 入口 | 用途 |
|---|---|---|
| `@mofukuma/discord` | `GDDiscord` | Discord の文字 Bot |
| `@mofukuma/memcached` | `GDMemcached` | Memcached client |
| `@mofukuma/supabase` | `GDSupabase` | Supabase Database と Auth |

詳しい使い方は [Discord](extensions/discord/README.md)、[Memcached](extensions/memcached/README.md)、[Supabase](extensions/supabase/README.md) にあります。

## 対応環境

- Godot 4.7 以降
- macOS arm64
- Linux x86_64
- Windows x86_64

native 拡張は process と同じ権限で動きます。信頼する package と version だけを固定し、`gd.lock` を commit してください。

## build

Godot 4.7 対応の godot-cpp を用意し、対象を選んで build します。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
uvx --from scons==4.10.1 scons godot_cpp=tmp/ref_godot_cpp out=tmp \
  platform=macos target=template_debug arch=arm64
```

生成物と中間 file は `tmp/` に置きます。CI は対応する 3 OS で debug と release を build します。
