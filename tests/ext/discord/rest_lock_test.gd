# 別processのSQLite write lock中もmain threadを止めずRESTをfail-closedにする。
extends RefCounted

var done := false # REST判定が終わったか
var reply := {} # REST判定の結果


# 非同期REST結果を受け取る。
func receive(value: Dictionary):
	reply = value
	done = true


# DB lockの待機をworker内の100 msで止める。
func main() -> int:
	var fake := preload("res://fake_server.gd").new()
	Engine.get_main_loop().root.add_child(fake)
	var ports: Dictionary = fake.start()
	var bot := GDDiscord.bot("lock-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	if bot == null:
		return 1
	bot.request("GET", "/after429").connect(receive)
	var last := Time.get_ticks_msec()
	var frame_max := 0
	var until := last + 2000
	while not done and Time.get_ticks_msec() < until:
		await Engine.get_main_loop().process_frame
		var now := Time.get_ticks_msec()
		frame_max = max(frame_max, now - last)
		last = now
	var ok := done and not reply.get("ok", true) and reply.get("error", "") == "REST rate database failed"
	ok = ok and fake.after_at == 0 and frame_max < 50
	bot.close()
	fake.queue_free()
	print("rest_lock_discord=", ok, " frame_max_ms=", frame_max)
	return 0 if ok else 1
