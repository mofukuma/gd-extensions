# Hello

C++やGDExtensionを使わない、1本の`.gd`だけで配れるpackageの最小実例です。

```sh
gd add hello gd:@mofukuma/hello@^0.1.3
```

```gdscript
const Hello := preload("res://vendor/hello/mod.gd")

func main():
	print(Hello.message("gd"))
	return 0
```

公開側は`gd.json`の`main`へ`src/mod.gd`、`include`へ`["src"]`を指定します。
入口directoryのtreeは利用側の`vendor/<呼び名>/`へ配置されます。
利用側が明示的にpreloadするmoduleなので、公開入口ではglobalな`class_name`を使いません。
