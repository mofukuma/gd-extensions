# 実memcachedへ主要commandを送り、互換性を確かめる試験。
extends SceneTree


# SceneTree開始後に通信試験を始める。
func _initialize():
	call_deferred("run")


# 環境指定portの実serverで保存、取得、counter、削除を確かめる。
func run():
	var port := OS.get_environment("MEMCACHED_PORT").to_int()
	var cache := GDMemcached.client("127.0.0.1", port, {"prefix": "gd-live:", "timeout_ms": 1000})
	if cache == null:
		quit(2)
		return
	var set_res: Dictionary = await cache.set("text", "real")
	var get_res: Dictionary = await cache.get("text")
	var raw := PackedByteArray([0, 13, 10, 255])
	var raw_set: Dictionary = await cache.set_raw("raw", raw)
	var raw_get: Dictionary = await cache.get("raw")
	var num_set: Dictionary = await cache.set("num", 2)
	var num_add: Dictionary = await cache.increment("num", 3)
	var added: Dictionary = await cache.add("conditional", "first")
	var replaced: Dictionary = await cache.replace("conditional", "second")
	var many: Dictionary = await cache.get_many(PackedStringArray(["text", "conditional", "missing"]))
	var ttl_set: Dictionary = await cache.set("ttl", "soon", 10)
	var touched: Dictionary = await cache.touch("ttl", 1)
	await create_timer(1.1).timeout
	var expired: Dictionary = await cache.get("ttl")
	var removed: Dictionary = await cache.remove("text")
	var passed: bool = set_res.ok and get_res.value == "real" and raw_set.ok and raw_get.value == raw and num_set.ok and num_add.value == 5
	passed = passed and added.ok and replaced.ok and many.hits == 2 and many.value.conditional == "second"
	passed = passed and ttl_set.ok and touched.hit and not expired.hit and removed.hit
	print("live_memcached=", passed)
	# native classのNodeと結果参照を終了前に解放する。
	cache.close()
	set_res = {}
	get_res = {}
	raw_set = {}
	raw_get = {}
	num_set = {}
	num_add = {}
	added = {}
	replaced = {}
	many = {}
	ttl_set = {}
	touched = {}
	expired = {}
	removed = {}
	cache = null
	await process_frame
	await process_frame
	call_deferred("finish", passed)


# 非同期frameを解放した後に終了する。
func finish(passed: bool):
	quit(0 if passed else 1)
