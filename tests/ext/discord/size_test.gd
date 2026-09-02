# 64 KiB pageの既存DBにも64 MiB上限を設定できるか確かめる。
extends RefCounted


# RESTを1件通して制限DBを初期化する。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("size-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	if bot == null:
		return 1
	var reply: Dictionary = await bot.request("GET", "/slow-frame")
	bot.close()
	fake.queue_free()
	print("size_discord=", reply.ok)
	return 0 if reply.ok else 1
