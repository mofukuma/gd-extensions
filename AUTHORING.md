# 自作packageの作り方

gdのpackageは二種類あります。まず純GDScriptで書けないか考えてください。

| | 純GDScript package (`gd:`) | GDExtension package (`ext:`) |
|---|---|---|
| 要る道具 | gdだけ | gd、C++ compiler、SCons、godot-cpp |
| 配る物 | `.gd` file | `.gdextension`とOSごとのnative library |
| 動くOS | 同じfileが全OSで動く | buildした組合せだけ |
| 利用側の入口 | 呼び名を決めて`preload` | manifestが宣言するglobal class |
| 壊れたとき | script errorで止まる | processごと落ちる |

gdの標準APIはTCP、TLS、HTTP、JSON、暗号、databaseを持つので、外部serviceのclientはたいていC++なしで書けます。
本repoの[Discord Bot](extensions/discord/README.md)はGatewayとRESTを純GDScriptだけで実装した実例です。
C++が要るのは、既存のCライブラリを結合するときと、GDScriptでは重すぎる計算だけです。

## 1. 純GDScript package

### 1.1 骨格

```sh
mkdir -p greet/src && cd greet
gd init
```

`gd init`が作った`gd.json`を書き換えます。

```json
{
  "name": "@luca/greet",
  "version": "0.1.0",
  "main": "src/mod.gd",
  "include": ["src"],
  "description": "挨拶を返すpackage"
}
```

- `name`は配るなら`@scope/name`。`gd init`の既定`my-tool`のままではpublishできません
- `main`は入口の`.gd`。登録所では常に`mod.gd`という名前で配られます
- `include`は`main`のdirectory内で一緒に配るfileまたはdirectory。省くと入口1本だけになります
- `version`はbuild metadataの無い完全なSemantic Version。package全体で500 MiBまでです

### 1.2 入口

```gdscript
# 利用側がpreloadする公開入口。
extends RefCounted

const Style = preload("style.gd") # package内は相対pathで参照する。


# 指定した名前への挨拶を返す。
static func message(name = "world"):
	return Style.decorate("hello, %s" % name)
```

- `class_name`を使いません。呼び名は利用側が決めて`preload`します
- package内のfileは**相対path**で`preload`します。treeは利用側の`vendor/<呼び名>/`へそのまま置かれるので、
  呼び名が何であっても解決できます。`res://vendor/greet/style.gd`と書くと呼び名を固定してしまいます
- 型注釈を書かない公開moduleでは、動的な戻り値へ`:=`を使いません（`var x = value`）。
  理由は[調査メモ](ログ/調査メモ_GDScript型推論の件.md)にあります

書けたら型検査と整形を通します。

```sh
gd check src
gd fmt --check src
```

### 1.3 配る三つの道

公式登録所`https://mofukuma.github.io/gd-extensions`は読取り専用です。自作packageは自分の配布先へ置きます。

**(a) 1 fileを直URLで配る。** 登録所が要りません。

```sh
gd add solo https://example.com/mod.gd
```

`vendor/solo.gd`へ置かれ、`gd.lock`へSHA-256が入ります。file 1本だけの道です。

**(b) 静的登録所。** 書込みserverが要らず、GitHub Pagesなどの静的配信で足ります。次の形を置きます。

```text
<registry>/-/catalog.json                  検索用の全件索引
<registry>/@luca/greet/meta.json           版とfileごとのsha256・size
<registry>/@luca/greet/0.1.0/mod.gd        配布する入口
```

本repoの`tools/registry.py`がrelease生成物からこの形を作ります。公式登録所もこの方式です。

**(c) 書込みできる登録所server。** `gd publish`が使えます。gdに同梱の`tools/registry.gd`をそのまま使えます。

```sh
gd --allow-net=127.0.0.1:8787 serve tools/registry.gd
```

`data/index.json`へ持ち主とtokenを登録します。keyはtoken本文のSHA-256です。

```json
{"packages":{},"classes":{},"tokens":{"<sha256(token)>":"luca"}}
```

publish側は`gd.json`へ`registry`を書き、tokenは環境変数だけで渡します。

```sh
GD_TOKEN=<token> gd --allow-net=127.0.0.1:8787 --allow-env=GD_TOKEN publish
```

利用側も同じ登録所を指し、呼び名を決めて追加します。

```sh
gd add greet gd:@luca/greet@^0.1.0
```

```gdscript
const Greet = preload("res://vendor/greet/mod.gd")


func main():
	print(Greet.message("gd"))
	return 0
```

版は不変です。同じ版を上書きできないので、直したら`version`を上げてから`gd publish`します。
利用側は`gd update <呼び名> --latest`で範囲ごと上げられます。

## 2. GDExtension package

### 2.1 godot-cpp

Godot 4.7に対応したcommitを固定して使います。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
```

### 2.2 C++

公開Singletonは`Object`、利用者が持ち回すinstanceは`RefCounted`にします。
`RefCounted`をSingletonへ登録すると、最後のRefが消えた時点でdangling pointerになります。

```cpp
// 挨拶文字列を返す公開Singleton。
class GDHello : public Object {
	GDCLASS(GDHello, Object)

protected:
	static void _bind_methods();

public:
	String message(const String &p_name) const; // 名前へ挨拶を返す
};
```

```cpp
// 公開methodをGDScriptから呼べるよう登録する。
void GDHello::_bind_methods() {
	ClassDB::bind_method(D_METHOD("message", "name"), &GDHello::message);
}
```

初期化入口の名前は、あとでmanifestの`entry_symbol`へ書く名前と一致させます。

```cpp
static GDHello *hello_api = nullptr; // 公開Singletonの実体

// classとSingletonを登録する。
void initialize_hello_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(GDHello);
	hello_api = memnew(GDHello);
	Engine::get_singleton()->register_singleton("GDHello", hello_api);
}

// Singletonを外してclassを解放可能にする。
void uninitialize_hello_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	Engine::get_singleton()->unregister_singleton("GDHello");
	memdelete(hello_api);
	hello_api = nullptr;
}

// engineから呼ばれるGDExtension初期化入口。
extern "C" GDExtensionBool GDE_EXPORT hello_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init(p_get_proc_address, p_library, r_initialization);
	init.register_initializer(initialize_hello_module);
	init.register_terminator(uninitialize_hello_module);
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init.init();
}
```

class名はClassDB全体で一つしか使えません。衝突すると読込みごと拒否されるので、
`GD`で始まる短い名前へ揃え、公開前に登録所へ予約してください。

### 2.3 build

`SConstruct`はgodot-cppのbuild環境をそのまま使います。

```python
#!/usr/bin/env python
# 最小GDExtensionをGodot 4.7向けにbuildする。

import os

godot_cpp = os.path.abspath(ARGUMENTS.get("godot_cpp", "../tmp/ref_godot_cpp"))  # godot-cppのsource
env = SConscript(os.path.join(godot_cpp, "SConstruct"), {"api_version": "4.7"})  # Godot 4.7用build環境
env.Append(CPPPATH=["src"])
sources = Glob("src/*.cpp")  # extensionへ含める実装file
target = "bin/libgdhello{}{}".format(env["suffix"], env["SHLIBSUFFIX"])  # platform別library名
Default(env.SharedLibrary(target, source=sources))
```

```sh
scons godot_cpp=../tmp/ref_godot_cpp platform=linux arch=x86_64 target=template_debug
```

配るには対応3 OSのdebugとreleaseを揃えます。生成されるlibrary名は
`platform`、`target`、`arch`から決まり、manifestへ書く名前と一致していなければなりません。

### 2.4 manifest

```ini
; GDHelloの公開class、platform library、await型を定義するmanifest。

[configuration]
; 初期化入口と読込み可能なGodot版。
entry_symbol = "hello_library_init"
compatibility_minimum = "4.7"
reloadable = true

[libraries]
; packageが機種ごとに選ぶnative library。
macos.debug.arm64 = "bin/libgdhello.macos.template_debug.arm64.dylib"
macos.release.arm64 = "bin/libgdhello.macos.template_release.arm64.dylib"
linux.debug.x86_64 = "bin/libgdhello.linux.template_debug.x86_64.so"
linux.release.x86_64 = "bin/libgdhello.linux.template_release.x86_64.so"
windows.debug.x86_64 = "bin/libgdhello.windows.template_debug.x86_64.dll"
windows.release.x86_64 = "bin/libgdhello.windows.template_release.x86_64.dll"

[classes]
; gdの型推論とcompileが追跡するclass。
GDHello = ""

[await]
; 非同期methodをawaitした後の型。
GDHelloClient.fetch = "Dictionary"

[result]
; Rを返すmethodの成功値の型。
GDHelloClient.open = "GDHelloSession"
```

`[classes]`、`[await]`、`[result]`はgd固有の節です。

- `[classes]`は読込み前の名前衝突判定に使います。宣言した名前が既にあると読込みを拒否します
- `[await]`は`await`した後の型を補います。signalの戻り値からは推論できないためです
- `[result]`は`R`を返すmethodの成功値の型を補います

`.gdextension`は16 MiBまでです。実例は
[memcached](extensions/memcached/memcached.gdextension)と[supabase](extensions/supabase/supabase.gdextension)にあります。

### 2.5 publishと利用

`gd.json`の`main`をmanifestにします。`include`は純GDScript packageだけの設定なので使えません。

```json
{
  "name": "@luca/hello-native",
  "version": "0.1.0",
  "main": "hello.gdextension",
  "description": "C++で書いた最小GDExtension"
}
```

`gd publish`はmanifestの`[libraries]`と`[dependencies]`に書いたfileを全部一緒に送ります。
先に全OS分をbuildしてから実行してください。

```sh
GD_TOKEN=<token> gd --allow-net=<登録所> --allow-env=GD_TOKEN publish
```

利用側は呼び名を付けません。manifestが宣言するclass名をそのまま使います。

```sh
gd add ext:@luca/hello-native@^0.1.0
```

```gdscript
func main():
	print(GDHello.message("gd"))
	return 0
```

```sh
gd --strict --allow-ext run main.gd
```

`gd install`は`vendor/ext/<name>/`へmanifestと**現在のOS向けlibraryだけ**を置き、
`.godot/extension_list.cfg`へ登録します。`gd.lock`は全OS分の指紋を保つので、
別のOSで`gd install`しても同じ内容が検証されます。

native拡張はprocessと同じ権限で動きます。読み込んだ時点でそのC++は何でもできるので、
信頼するpackageと版だけを`gd.lock`で固定し、それをcommitしてください。
