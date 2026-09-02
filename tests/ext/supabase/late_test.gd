# Supabase GDExtensionを実行中にloadし、通信後にunloadする試験。
extends SceneTree

const EXT := "res://supabase.gdextension" # 実行中に読むmanifest

var checks := 0 # 実行した検査数
var failures := 0 # 失敗した検査数


# 条件を数え、失敗内容を表示する。
func check(condition: bool, label: String):
	checks += 1
	if not condition:
		failures += 1
		print("FAIL ", label)


# local APIを通してload、Database、Auth、unloadを確かめる。
func _initialize():
	call_deferred("run")


# 全試験を順番に実行する。
func run():
	var fake := preload("res://fake_server.gd").new()
	root.add_child(fake)
	var port := fake.start()
	check(port > 0, "fake server")
	check(not Engine.has_singleton("GDSupabase"), "singleton absent before load")

	var loaded := GDExtensionManager.load_extension(EXT)
	check(loaded == GDExtensionManager.LOAD_STATUS_OK, "load extension")
	check(Engine.has_singleton("GDSupabase"), "singleton registered")
	var api = Engine.get_singleton("GDSupabase")
	var sb = api.call("client", "http://127.0.0.1:%d" % port, "sb_publishable_test")
	check(sb != null, "client created")

	var selected: Dictionary = await sb.call("select", "items", {"select": "id,title", "limit": 2})
	check(selected.ok, "select")
	check(selected.data[0].apikey == "sb_publishable_test", "apikey header")
	check(selected.data[0].auth == "", "publishable key is not bearer")
	check("select=id%2Ctitle" in selected.data[0].path, "query encoded")
	var settings: Dictionary = await sb.call("auth_settings")
	check(settings.ok and settings.data.has("disable_signup"), "auth settings")

	var inserted: Dictionary = await sb.call("insert", "items", {"title": "hello"})
	check(inserted.ok and inserted.data[0].method == "POST", "insert")
	var updated: Dictionary = await sb.call("update", "items", {"title": "new"}, {"id": "eq.1"})
	check(updated.ok and updated.data[0].method == "PATCH", "update")
	var removed: Dictionary = await sb.call("remove", "items", {"id": "eq.1"})
	check(removed.ok and removed.data[0].method == "DELETE", "remove")
	var rpc: Dictionary = await sb.call("rpc", "add", {"a": 2, "b": 3})
	check(rpc.ok and rpc.data.sum == 5, "rpc")
	var missing: Dictionary = await sb.call("select", "missing")
	check(not missing.ok and missing.status == 404 and missing.error == "not found", "REST error")

	var signed: Dictionary = await sb.call("sign_in", "test@example.com", "password")
	check(signed.ok and sb.get("session").access_token == "access-test", "sign in")
	var authed: Dictionary = await sb.call("select", "items")
	check(authed.data[0].auth == "Bearer access-test", "session bearer")
	var refreshed: Dictionary = await sb.call("refresh")
	check(refreshed.ok and sb.get("session").access_token == "access-refreshed", "refresh")
	var signed_out: Dictionary = await sb.call("sign_out")
	check(signed_out.ok and sb.get("session").is_empty(), "sign out")

	# extension instanceを全て離してからnative libraryを外す。
	selected = {}
	settings = {}
	inserted = {}
	updated = {}
	removed = {}
	rpc = {}
	missing = {}
	signed = {}
	authed = {}
	refreshed = {}
	signed_out = {}
	sb = null
	api = null
	fake.queue_free()
	await process_frame
	await process_frame
	var unloaded := GDExtensionManager.unload_extension(EXT)
	check(unloaded == GDExtensionManager.LOAD_STATUS_OK, "unload extension")
	check(not Engine.has_singleton("GDSupabase"), "singleton removed")
	print("checks=%d failures=%d" % [checks, failures])
	quit(failures)
