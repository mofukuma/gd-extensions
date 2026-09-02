# 起動時loadしたMemcached GDExtensionを通常終了できるか確かめる。
extends SceneTree


# Singleton確認後に通常のengine終了へ進む。
func _initialize():
	call_deferred("run")


# 起動時登録と通信後のclient解放を確認し、明示unloadせず終了する。
func run():
	var loaded := Engine.has_singleton("GDMemcached")
	var fake := preload("res://fake_server.gd").new()
	root.add_child(fake)
	var cache := GDMemcached.client("127.0.0.1", fake.start())
	var stored: Dictionary = await cache.set("exit", "safe")
	var found: Dictionary = await cache.get("exit")
	var passed: bool = loaded and stored.ok and found.value == "safe"
	cache.close()
	stored = {}
	found = {}
	cache = null
	fake.queue_free()
	await process_frame
	await process_frame
	print("startup_memcached=", passed)
	call_deferred("finish", passed)


# 非同期frameを解放した後に終了する。
func finish(passed: bool):
	quit(0 if passed else 1)
