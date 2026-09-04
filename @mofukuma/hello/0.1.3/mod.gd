# C++を使わずinstallできる純GDScript moduleの最小実例。
extends RefCounted


# 指定した名前への挨拶を返す。
static func message(name = "gd"):
	return "hello, %s" % name
