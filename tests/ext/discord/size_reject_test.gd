# 64 MiBを越えた既存DBを制限処理が拒否するか確かめる。
extends RefCounted


# RESTがDB上限でfail-closedになることを確かめる。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("size-reject-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	if bot == null:
		return 1
	var reply: Dictionary = await bot.request("GET", "/slow-frame")
	var ok := not reply.ok and reply.error == "REST rate database failed" and fake.slow_count == 0
	bot.close()
	fake.queue_free()
	print("size_reject_discord=", ok)
	return 0 if ok else 1
