# Memcached GDExtensionを実行中にloadし、通信後にunloadする試験。
extends SceneTree

const EXT := "res://memcached.gdextension" # 実行中に読むmanifest

var checks := 0 # 実行した検査数
var failures := 0 # 失敗した検査数


# 条件を数え、失敗内容を表示する。
func check(condition: bool, label: String):
	checks += 1
	if not condition:
		failures += 1
		print("FAIL ", label)


# SceneTree開始後に試験を実行する。
func _initialize():
	call_deferred("run")


# 最初のSignal完了時にclientを閉じ、待ち要求の再入通知を作る。
func close_after(done_signal: Signal, cache):
	await done_signal
	cache.call("close")


# load、codec、主要command、persistent接続、unloadを確かめる。
func run():
	var fake := preload("res://fake_server.gd").new()
	root.add_child(fake)
	var port := fake.start()
	check(port > 0, "fake server")
	check(not Engine.has_singleton("GDMemcached"), "singleton absent before load")

	var loaded := GDExtensionManager.load_extension(EXT)
	check(loaded == GDExtensionManager.LOAD_STATUS_OK, "load extension")
	check(Engine.has_singleton("GDMemcached"), "singleton registered")
	var api = Engine.get_singleton("GDMemcached")
	var cache = api.call("client", "127.0.0.1", port, {"prefix": "test:", "timeout_ms": 250})
	check(cache != null, "client created")

	var stored: Dictionary = await cache.call("set", "text", "hello")
	check(stored.ok, "set string")
	var text: Dictionary = await cache.call("get", "text")
	check(text.ok and text.hit and text.value == "hello", "get string")
	var json: Dictionary = await cache.call("set", "json", {"id": 7, "tags": ["a", "b"]})
	var got_json: Dictionary = await cache.call("get", "json")
	check(json.ok and got_json.value.id == 7 and got_json.value.tags.size() == 2, "json codec")
	var raw := PackedByteArray([0, 13, 10, 255])
	var raw_set: Dictionary = await cache.call("set_raw", "raw", raw, 99)
	var raw_get: Dictionary = await cache.call("get", "raw")
	check(raw_set.ok and raw_get.value == raw and raw_get.flags == 99, "binary safe raw value")

	var counter_set: Dictionary = await cache.call("set", "counter", 10)
	var increased: Dictionary = await cache.call("increment", "counter", 5)
	var decreased: Dictionary = await cache.call("decrement", "counter", 20)
	check(counter_set.ok and increased.value == 15 and decreased.value == 0, "counter commands")
	var added: Dictionary = await cache.call("add", "once", "first")
	var conflict: Dictionary = await cache.call("add", "once", "second")
	var replaced: Dictionary = await cache.call("replace", "once", "new")
	check(added.ok and not conflict.ok and conflict.kind == "not_stored" and replaced.ok, "conditional stores")

	var many: Dictionary = await cache.call("get_many", PackedStringArray(["text", "once", "missing"]))
	check(many.ok and many.hits == 2 and many.value.text == "hello" and many.value.once == "new", "get many one round trip")
	var touched: Dictionary = await cache.call("touch", "once", 10)
	var removed: Dictionary = await cache.call("remove", "once")
	var missed: Dictionary = await cache.call("get", "once")
	check(touched.hit and removed.hit and missed.ok and not missed.hit, "touch delete miss")
	var version: Dictionary = await cache.call("version")
	check(version.ok and version.value == "fake-1.0", "version")
	var invalid: Dictionary = await cache.call("get", "bad key")
	check(not invalid.ok and invalid.kind == "invalid_data", "invalid key")
	check(fake.accepted == 1, "persistent TCP connection")
	var stalled: Dictionary = await cache.call("set", "stall", "unknown")
	check(not stalled.ok and stalled.kind == "ambiguous", "sent write timeout is ambiguous")
	var reconnected: Dictionary = await cache.call("version")
	check(reconnected.ok and fake.accepted == 2, "timeout connection is replaced")
	var named = api.call("client", "localhost", port, {"timeout_ms": 250, "ip_type": "ipv4"})
	var named_version: Dictionary = await named.call("version")
	check(named_version.ok, "hostname resolves asynchronously")
	named.call("close")
	named = null

	# 完了callbackと待ち要求callbackの双方からcloseしても二重解放しない。
	var closing = api.call("client", "127.0.0.1", port, {"prefix": "close:", "timeout_ms": 250})
	var first: Signal = closing.call("set", "first", "a")
	var second: Signal = closing.call("set", "second", "b")
	close_after(first, closing)
	var interrupted: Dictionary = await second
	closing.call("close")
	check(not interrupted.ok and interrupted.kind == "interrupted", "close is reentrant")
	closing = null

	# 件数上限前でもpayload総byte上限でqueueを拒否する。
	var limited = api.call("client", "127.0.0.1", port, {"max_value": 64, "max_response": 1024, "max_queue_bytes": 576})
	var payload := PackedByteArray()
	payload.resize(64)
	var last: Signal
	for i in 8:
		last = limited.call("set_raw", "key%d" % i, payload)
	var over: Dictionary = await last
	check(not over.ok and over.kind == "limited", "queue byte limit")
	limited.call("close")
	limited = null

	# 壊れたserver応答を接続ごと破棄し、分類して返す。
	var broken: Dictionary
	for key in ["bad_value", "bad_size", "bad_term", "foreign", "bad_flags"]:
		broken = await cache.call("get", key)
		check(not broken.ok and broken.kind == "protocol", "reject " + key)
	broken = await cache.call("remove", "unexpected")
	check(not broken.ok and broken.kind == "protocol", "reject unexpected line")
	var capped = api.call("client", "127.0.0.1", port, {"max_value": 1, "max_response": 513})
	var capped_res: Dictionary = await capped.call("get", "overflow")
	check(not capped_res.ok and capped_res.kind == "limited", "response byte limit")
	capped.call("close")
	capped = null
	var cumulative = api.call("client", "127.0.0.1", port, {"max_value": 200, "max_response": 712})
	var cumulative_res: Dictionary = await cumulative.call("get", "cumulative")
	check(not cumulative_res.ok and cumulative_res.kind == "limited", "cumulative response byte limit")
	cumulative.call("close")
	cumulative = null

	# extension instanceを全て離してからnative libraryを外す。
	stored = {}
	text = {}
	json = {}
	got_json = {}
	raw_set = {}
	raw_get = {}
	counter_set = {}
	increased = {}
	decreased = {}
	added = {}
	conflict = {}
	replaced = {}
	many = {}
	touched = {}
	removed = {}
	missed = {}
	version = {}
	invalid = {}
	stalled = {}
	reconnected = {}
	named_version = {}
	interrupted = {}
	over = {}
	broken = {}
	capped_res = {}
	cumulative_res = {}
	cache.call("close")
	cache = null
	api = null
	fake.queue_free()
	await process_frame
	await process_frame
	var unloaded := GDExtensionManager.unload_extension(EXT)
	check(unloaded == GDExtensionManager.LOAD_STATUS_OK, "unload extension")
	check(not Engine.has_singleton("GDMemcached"), "singleton removed")
	print("checks=%d failures=%d" % [checks, failures])
	quit(failures)
