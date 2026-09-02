# 別processでもSQLiteのIdentify上限が残るか確かめる。
extends RefCounted


# 前processで1件消費したtokenが再Identifyできないことを確認する。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("stored-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		"identify_limit": 1,
	})
	if bot == null or bot.start() != OK:
		return 1
	var hello_until := Time.get_ticks_msec() + 8000
	while fake.hello_count == 0 and Time.get_ticks_msec() < hello_until:
		await Engine.get_main_loop().process_frame
	var identify_until := Time.get_ticks_msec() + 5200
	while Time.get_ticks_msec() < identify_until:
		await Engine.get_main_loop().process_frame
	var ok := fake.hello_count == 1 and fake.identify_count == 0
	bot.close()
	fake.queue_free()
	print("persistent_discord=", ok)
	return 0 if ok else 1
