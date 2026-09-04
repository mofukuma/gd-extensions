# Hello

C++やGDExtensionを使わない、1本の`.gd`だけで配れるpackageの最小実例です。

```sh
gd add hello gd:@mofukuma/hello@^0.1.1
```

```gdscript
const Hello := preload("res://vendor/hello.gd")

func main():
	print(Hello.message("gd"))
	return 0
```

公開側は`gd.json`の`main`へ任意の`.gd`を指定します。登録所では`mod.gd`へ正規化され、利用側では`vendor/<呼び名>.gd`へ配置されます。
