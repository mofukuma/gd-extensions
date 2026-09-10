# Hello

C++やGDExtensionを使わない、1本の`.gd`だけで配れるpackageの最小実例です。

```sh
gd add hello gd:@mofukuma/hello@^0.1.3
```

```gdscript
const Hello := preload("pkg://hello/mod.gd")

func main():
	print(Hello.message("gd"))
	return 0
```

公開側は`gd.json`の`main`へ`src/mod.gd`、`include`へ`["src"]`を指定します。
入口directoryのtreeがそのまま`pkg://<呼び名>/`から読めます。本家Godotでは`"place": "project"`で`pkg/<呼び名>/`へ置き、`res://pkg/<呼び名>/mod.gd`で読みます。
利用側が明示的にpreloadするmoduleなので、`class_name`を使いません。書くと`gd publish`が拒みます。
