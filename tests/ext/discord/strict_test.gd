# strict権限下でlocalhostのGatewayとRESTを使えるか確かめる。
extends RefCounted


# localhostを名前で許可し、IP接続と待受を一巡する。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("strict-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	if bot == null or bot.start() != OK:
		return 1
	await bot.ready
	var reply: Dictionary = await bot.send_message("123", "strict REST")
	bot.close()
	fake.queue_free()
	print("strict_discord=", reply.ok)
	return 0 if reply.ok else 1
