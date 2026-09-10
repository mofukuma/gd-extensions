# gd extensions

gd と Godot 4.7 から使える公式 package 集です。native 拡張の公開 API は `GD` から始まる短い名前に揃えています。

## 使う

gd 0.7.3 以降で、project の中から必要な拡張だけ追加します。
既定の公式登録所は `https://mofukuma.github.io/gd-extensions` です。

```sh
gd search discord
gd add hello gd:@mofukuma/hello@^0.1.3
gd add discord gd:@mofukuma/discord@^0.1.3
gd add ext:@mofukuma/memcached@^0.1.3
gd add ext:@mofukuma/supabase@^0.1.3
```

純GDScript packageは共有cacheに置いたまま`pkg://<呼び名>/`で読みます。検証したnative libraryとmanifestは`pkg/<呼び名>/`へ置きます。
本家Godotから読むときは`gd.json`へ`"place": "project"`を書くと純GDScript packageも`pkg/<呼び名>/`へ置かれ、`res://pkg/<呼び名>/mod.gd`で読めます。
packageが自分の`gd.json`の`imports`で使う他のpackageも一緒に解決され、`gd.lock`に版ごと固定されます。
他のpackageだけが使うものは`pkg/@scope/name@版/`に置かれます。

| package | 入口 | 用途 |
|---|---|---|
| `@mofukuma/hello` | `preload("pkg://hello/mod.gd")` | C++なしの純GDScript package実例 |
| `@mofukuma/discord` | `preload("pkg://discord/mod.gd")` | Discord の文字 Bot |
| `@mofukuma/memcached` | `GDMemcached` | Memcached client |
| `@mofukuma/supabase` | `GDSupabase` | Supabase Database と Auth |

詳しい使い方は [Hello](extensions/hello/README.md)、[Discord](extensions/discord/README.md)、[Memcached](extensions/memcached/README.md)、[Supabase](extensions/supabase/README.md) にあります。

## 対応環境

- Godot 4.7 以降
- macOS arm64
- Linux x86_64
- Windows x86_64

native 拡張は process と同じ権限で動きます。信頼する package と version だけを固定し、`gd.lock` を commit してください。

公式登録所は読取り専用です。自作packageの`gd publish`には、`gd.json`の`registry`で自分の書込み可能な登録所を指定してください。

## 自作する

自分のpackageを作って配る手順は[自作packageの作り方](AUTHORING.md)にあります。
純GDScriptならgdだけで作れます。C++が要るときのGDExtensionの道も同じ文書にあります。

## build

Godot 4.7 対応の godot-cpp を用意し、対象を選んで build します。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
uvx --from scons==4.10.1 scons godot_cpp=tmp/ref_godot_cpp out=tmp \
  build_profile=tools/build_profile.json platform=macos target=template_debug arch=arm64
```

生成物と中間 file は `tmp/` に置きます。CI は対応する3 OSでdebugを、release時はdebugとreleaseをbuildします。
