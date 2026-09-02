# 別processへ引き継ぐREST global停止をSQLiteへ記録する。
extends RefCounted


# 2秒のglobal 429を1回受け、再試行前にprocessを終える。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	fake.rest_retry = 2.0
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("rest-process-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	if bot == null:
		return 1
	var pending = bot.request("GET", "/always429")
	var until := Time.get_ticks_msec() + 2000
	while fake.limited_count == 0 and Time.get_ticks_msec() < until:
		await Engine.get_main_loop().process_frame
	await Engine.get_main_loop().process_frame
	var ok := fake.limited_count == 1 and pending is Signal
	print("rest_seed_discord=", ok)
	return 0 if ok else 1
