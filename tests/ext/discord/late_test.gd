# Discord純GDScript moduleのGatewayとRESTをlocalhostで一巡する。
extends SceneTree

const Discord := preload("res://discord/mod.gd") # 利用者と同じ明示preload入口

var checks := 0 # 実行した検査数
var failures := 0 # 失敗した検査数


# 条件を数え、失敗内容を表示する。
func check(condition, label):
	checks += 1
	if not condition:
		failures += 1
		print("FAIL ", label)


# local fake serverを使う試験を遅延開始する。
func _initialize():
	call_deferred("run")


# 条件成立まで最大8秒frameを進める。
func wait_for(done):
	var until := Time.get_ticks_msec() + 8000
	while Time.get_ticks_msec() < until:
		if done.call():
			return true
		await process_frame
	return false


# Signalへ直ちに繋ぎ、後から待てる結果箱を返す。
func watch(value):
	var state := {"done": false, "value": {}}
	value.connect(func(reply):
		state.done = true
		state.value = reply
	, CONNECT_ONE_SHOT)
	return state


# 結果箱を最大8秒待ってDictionaryを返す。
func wait_reply(state):
	if await wait_for(func(): return state.done):
		return state.value
	return {"ok": false, "status": 0, "data": {}, "error": "test timeout"}


# 公開API、Gateway、REST、rate limit、終了処理を検証する。
func run():
	var fake := preload("res://fake_server.gd").new()
	root.add_child(fake)
	var ports = fake.start()
	check(not ports.is_empty(), "fake server")
	check(Discord.GUILDS == 1 and Discord.MESSAGE_CONTENT == 32768, "intent constants")
	check(Discord.bot("test", {"api_url": "http://example.com/api/v10"}) == null, "remote plaintext rejected")
	check(Discord.bot("test", {"api_url": "https://example.com/api/v10"}) == null, "remote override rejected")
	check(Discord.bot("bad\ntoken") == null, "token header injection rejected")
	check(Discord.bot("test", {"intents": -1}) == null, "negative intents rejected")
	check(Discord.bot("test", {"max_rest_queue": 1025}) == null, "REST queue count bounded")
	check(Discord.bot("test", {"max_gateway_packet": 16 * 1024 * 1024}) == null, "packet limit bounded")

	var bot = Discord.bot("test-token", {
		"intents": 33281,
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	check(bot != null, "bot created")
	var seen := {"ready": false, "message": false, "resumed": false}
	bot.ready.connect(func(_data): seen.ready = true)
	bot.event.connect(func(name, data):
		if name == "MESSAGE_CREATE" and data.content == "hello":
			seen.message = true
	)
	bot.resumed.connect(func(): seen.resumed = true)
	check(bot.start() == OK, "gateway start")
	check(await wait_for(func(): return seen.ready and seen.message and seen.resumed), "ready event and resume")
	check(fake.gateway_bot_count == 1, "gateway information fetched")
	check(fake.identify_token == "test-token", "identify token")
	check(bot.get_session_id() == "session-test" and bot.is_ready(), "session retained")
	check(bot.set_presence({"since": null, "activities": [], "status": "online", "afk": false}) == OK, "presence queued")
	check(await wait_for(func(): return fake.presence_count == 1), "presence sent")

	var sent = await wait_reply(watch(bot.send_message("123", "hello REST")))
	check(sent.ok and sent.status == 200, "message sent after 429")
	check(sent.data.content == "hello REST", "message body")
	check(sent.data.mentions.parse.is_empty(), "mentions disabled")
	check(sent.data.auth == "Bot test-token", "authorization header")
	check(fake.rest_count == 2, "429 retried once")
	var invalid = await wait_reply(watch(bot.request("TRACE", "/bad")))
	check(not invalid.ok and invalid.error == "unsupported HTTP method", "method rejected")

	var small = Discord.bot("small-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		"max_queue_bytes": 1024,
	})
	var oversized = await wait_reply(watch(small.send_message("123", "x".repeat(2048))))
	check(not oversized.ok and oversized.error == "REST queue is full", "queue bytes bounded")
	small.close()

	bot.close()
	var closed = await wait_for(func(): return fake.close_code == 1000)
	print("close_code=", fake.close_code)
	check(closed, "clean gateway close")
	var after_close = await wait_reply(watch(bot.request("GET", "/after429")))
	check(not after_close.ok and after_close.error == "client closed", "request after close completed")
	fake.queue_free()
	await process_frame
	print("checks=%d failures=%d" % [checks, failures])
	quit(failures)
