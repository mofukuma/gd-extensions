# installした純GDScript packageを利用側と同じpreloadで確かめる。
extends SceneTree

const Hello := preload("res://vendor/hello/mod.gd")


# moduleの公開関数を実行して終了する。
func _init():
	var got = Hello.message("ecosystem")
	print("gd_package=%s" % got)
	quit(0 if got == "hello, ecosystem" else 1)
