# 別processがSQLiteのREST global停止を引き継ぐか確かめる。
extends RefCounted


# 前processの停止解除まで送信されないことを確認する。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("rest-process-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	if bot == null:
		return 1
	var started := Time.get_ticks_msec()
	var reply: Dictionary = await bot.request("GET", "/after429")
	var elapsed := Time.get_ticks_msec() - started
	var ok := reply.ok and elapsed >= 800
	bot.close()
	fake.queue_free()
	print("rest_process_discord=", ok, " elapsed_ms=", elapsed)
	return 0 if ok else 1
