# Discord GDExtensionを実行中にloadし、Gateway、REST、unloadを確かめる。
extends SceneTree

const EXT := "res://discord.gdextension" # 実行中に読むmanifest

var checks := 0 # 実行した検査数
var failures := 0 # 失敗した検査数
var ready_seen := false # READY signal受信済み
var message_seen := false # MESSAGE_CREATE受信済み
var resumed_seen := false # RESUMED signal受信済み
var release_bot # ready callback中に最後の参照を外すclient


# 条件を数え、失敗内容を表示する。
func check(condition: bool, label: String):
	checks += 1
	if not condition:
		failures += 1
		print("FAIL ", label)


# local APIを通してload、Gateway、REST、unloadを確かめる。
func _initialize():
	call_deferred("run")


# READYを受けたことを控える。
func _ready_event(_data: Dictionary):
	ready_seen = true


# dispatchを受け、MESSAGE_CREATEを控える。
func _gateway_event(name: String, data):
	if name == "MESSAGE_CREATE" and data.content == "hello":
		message_seen = true


# RESUMEDを受けたことを控える。
func _resumed_event():
	resumed_seen = true


# ready signal中にclientの最後の利用側参照を外す。
func _release_ready(_data: Dictionary):
	release_bot = null


# 条件成立まで最大8秒frameを進める。
func wait_for(done: Callable) -> bool:
	var until := Time.get_ticks_msec() + 8000
	while Time.get_ticks_msec() < until:
		if done.call():
			return true
		await process_frame
	return false


# Signalへ直ちに繋ぎ、後から待てる結果箱を返す。
func watch(signal_value: Signal) -> Dictionary:
	var state := {"done": false, "value": {}}
	signal_value.connect(func(value: Dictionary):
		state.done = true
		state.value = value
	, CONNECT_ONE_SHOT)
	return state


# 結果箱を最大8秒待ってDictionaryを返す。
func wait_reply(state: Dictionary) -> Dictionary:
	if await wait_for(func(): return state.done):
		return state.value
	return {"ok": false, "status": 0, "data": {}, "error": "test timeout"}


# Bot通信を順に検証する。
func run():
	var fake := preload("res://fake_server.gd").new()
	root.add_child(fake)
	var ports := fake.start()
	check(not ports.is_empty(), "fake server")
	check(not Engine.has_singleton("GDDiscord"), "singleton absent before load")

	var loaded := GDExtensionManager.load_extension(EXT)
	check(loaded == GDExtensionManager.LOAD_STATUS_OK, "load extension")
	check(Engine.has_singleton("GDDiscord"), "singleton registered")
	var api = Engine.get_singleton("GDDiscord")
	var insecure = api.call("bot", "test-token", {
		"api_url": "http://example.com/api/v10",
	})
	check(insecure == null, "remote plaintext rejected")
	var remote_tls = api.call("bot", "test-token", {
		"api_url": "https://example.com/api/v10",
	})
	check(remote_tls == null, "remote TLS override rejected")
	var bot = api.call("bot", "test-token", {
		"intents": 33281,
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	check(bot != null, "bot created")
	var excessive = api.call("bot", "test-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		"max_gateway_packet": 16 * 1024 * 1024,
	})
	check(excessive == null, "gateway packet limit bounded")
	var excessive_identify = api.call("bot", "test-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		"identify_limit": 1000001,
	})
	check(excessive_identify == null, "identify limit bounded")
	bot.ready.connect(_ready_event)
	bot.event.connect(_gateway_event)
	bot.resumed.connect(_resumed_event)
	check(bot.call("start") == OK, "gateway start")
	check(await wait_for(func(): return ready_seen and message_seen and resumed_seen), "ready event and resume")
	check(fake.gateway_bot_count == 1, "gateway bot information fetched")
	check(fake.identify_token == "test-token", "identify token")
	check(fake.resume_count == 1, "resume used")
	check(await wait_for(func(): return fake.identify_count == 2 and bot.call("get_session_id") == "session-test"), "invalid sequence identified anew")
	check(bot.call("get_session_id") == "session-test", "session retained")
	check(bot.call("set_presence", {"since": null, "activities": [], "status": "online", "afk": false}) == OK, "presence queued")
	check(await wait_for(func(): return fake.presence_count == 1), "presence sent")

	# invalid応答はIP共有の10分枠へ残し、設定上限後の送信を止める。
	var invalid_bot = api.call("bot", "invalid-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		"invalid_limit": 1,
	})
	var forbidden: Dictionary = await wait_reply(watch(invalid_bot.call("request", "GET", "/forbidden")))
	check(not forbidden.ok and forbidden.status == 403, "invalid response counted")
	invalid_bot.call("request", "GET", "/forbidden")
	var invalid_until := Time.get_ticks_msec() + 300
	while Time.get_ticks_msec() < invalid_until:
		await process_frame
	check(fake.forbidden_count == 1, "invalid response limit shared")
	invalid_bot.call("close")
	var pending_fps := Engine.max_fps
	Engine.max_fps = 5
	var pending_a = api.call("bot", "pending-a", {"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest, "invalid_limit": 2})
	var pending_b = api.call("bot", "pending-b", {"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest, "invalid_limit": 2})
	var pending_a_reply := watch(pending_a.call("request", "GET", "/pending"))
	var pending_b_reply := watch(pending_b.call("request", "GET", "/pending"))
	var pending_a_value: Dictionary = await wait_reply(pending_a_reply)
	var pending_b_value: Dictionary = await wait_reply(pending_b_reply)
	Engine.max_fps = pending_fps
	check(pending_a_value.ok and pending_b_value.ok and fake.pending_count == 2, "pending invalid reservation rechecked")
	pending_a.call("close")
	pending_b.call("close")
	var auth_bot = api.call("bot", "auth-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	var unauthorized: Dictionary = await wait_reply(watch(auth_bot.call("request", "GET", "/unauthorized")))
	check(not unauthorized.ok and unauthorized.status == 401, "authorization failure received")
	var denied: Dictionary = await wait_reply(watch(auth_bot.call("request", "GET", "/slow-frame")))
	check(not denied.ok and fake.unauthorized_count == 1, "authorization failure stops client")
	auth_bot.call("close")

	# 5 FPSでもworkerのpermitを次frameで使える。
	var old_fps := Engine.max_fps
	Engine.max_fps = 5
	var slow := {"done": false, "reply": {}}
	bot.call("request", "GET", "/slow-frame").connect(func(reply: Dictionary):
		slow.done = true
		slow.reply = reply
	)
	var slow_done := await wait_for(func(): return slow.done)
	Engine.max_fps = old_fps
	check(slow_done and slow.reply.get("ok", false) and fake.slow_count == 1, "REST permit works at 5 FPS")

	# global 429は別clientも同じtokenの再開まで待つ。
	var sent_wait := watch(bot.call("send_message", "123", "hello REST"))
	check(await wait_for(func(): return fake.rest_count == 1), "global limit received")
	await process_frame
	var global_bot = api.call("bot", "test-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	var global_wait := watch(global_bot.call("request", "GET", "/after429"))
	var sent: Dictionary = await wait_reply(sent_wait)
	var global_reply: Dictionary = await wait_reply(global_wait)
	check(sent.ok and sent.status == 200, "message sent")
	check(sent.data.content == "hello REST", "message body")
	check(sent.data.mentions.parse.is_empty(), "string message mentions disabled")
	check(sent.data.auth == "Bot test-token", "bot authorization")
	check(fake.rest_count == 2, "429 retried once")
	check(global_reply.ok and fake.after_at - fake.global_at >= 45, "global limit shared by clients")
	global_bot.call("close")
	var limited_wait := watch(bot.call("request", "GET", "/always429"))
	var after_wait := watch(bot.call("request", "GET", "/after429"))
	var limited: Dictionary = await wait_reply(limited_wait)
	var after: Dictionary = await wait_reply(after_wait)
	check(not limited.ok and limited.status == 429 and fake.limited_count == 5, "429 retry bounded")
	check(after.ok and fake.after_at - fake.limited_at >= 45, "retry-after kept after final 429")
	var seeded: Dictionary = await wait_reply(watch(bot.call("request", "GET", "/bucket/use")))
	var exhausted: Dictionary = await wait_reply(watch(bot.call("request", "GET", "/bucket/exhaust")))
	var bucket_bot = api.call("bot", "test-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
	})
	var shared: Dictionary = await wait_reply(watch(bucket_bot.call("request", "GET", "/bucket/use")))
	check(seeded.ok and exhausted.ok and shared.ok, "shared bucket calls")
	check(fake.bucket_use_at - fake.bucket_at >= 45, "shared bucket waited across clients")
	bucket_bot.call("close")
	# runner固有のframe時間を直前に測り、REST負荷による相対的な停止だけを判定する。
	var base_frame_ms: Array[int] = []
	var base_last := Time.get_ticks_msec()
	var base_until := base_last + 1000
	while Time.get_ticks_msec() < base_until:
		await process_frame
		var base_now := Time.get_ticks_msec()
		base_frame_ms.append(base_now - base_last)
		base_last = base_now
	base_frame_ms.sort()
	var base_p95_at := maxi(0, ceili(base_frame_ms.size() * 0.95) - 1)
	var base_frame_p95 := base_frame_ms[base_p95_at] if not base_frame_ms.is_empty() else 8000
	# 46個のclientが一斉に送っても45件の次は予約保持分だけ待つ。
	var burst_clients := []
	for i in 46:
		var burst = api.call("bot", "burst-token", {
			"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		})
		burst_clients.append(burst)
		burst.call("request", "GET", "/global/%d" % i)
	var burst_last := Time.get_ticks_msec()
	var burst_frame_max := 0
	var burst_frame_ms: Array[int] = []
	var burst_frames := 0
	var burst_until := burst_last + 8000
	while fake.rest_global_at.size() < 46 and Time.get_ticks_msec() < burst_until:
		await process_frame
		burst_frames += 1
		var burst_now := Time.get_ticks_msec()
		var burst_frame := burst_now - burst_last
		burst_frame_ms.append(burst_frame)
		burst_frame_max = max(burst_frame_max, burst_frame)
		burst_last = burst_now
	check(fake.rest_global_at.size() == 46, "global count completed")
	var burst_delta := fake.rest_global_at[45] - fake.rest_global_at[0] if fake.rest_global_at.size() == 46 else 0
	burst_frame_ms.sort()
	var burst_p95_at := maxi(0, ceili(burst_frame_ms.size() * 0.95) - 1)
	var burst_frame_p95 := burst_frame_ms[burst_p95_at] if not burst_frame_ms.is_empty() else 8000
	print("rest_global_delta_ms=", burst_delta)
	print("rest_base_frame_p95_ms=", base_frame_p95)
	print("rest_global_frame_max_ms=", burst_frame_max)
	print("rest_global_frame_p95_ms=", burst_frame_p95)
	check(burst_delta >= 1500, "global count shared by clients")
	# 95% frame時間が平常時の2倍を越えたら、SQLite worker待ちによる退行として扱う。
	check(burst_frames >= 10 and burst_frame_p95 <= maxi(1, base_frame_p95 * 2), "REST SQLite keeps main loop running")
	for burst in burst_clients:
		burst.call("close")
	var invalid: Dictionary = await wait_reply(watch(bot.call("request", "TRACE", "/bad")))
	check(not invalid.ok and invalid.error == "unsupported HTTP method", "method rejected")
	var small = api.call("bot", "test-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % ports.rest,
		"max_queue_bytes": 1024,
	})
	var oversized: Dictionary = await wait_reply(watch(small.call("send_message", "123", "x".repeat(2048))))
	check(not oversized.ok and oversized.error == "REST queue is full", "REST byte queue bounded")
	small.call("close")

	bot.call("close")
	bot = null
	check(await wait_for(func(): return fake.close_code == 1000), "clean gateway close")
	fake.queue_free()
	await process_frame
	await process_frame

	# DiscordがIdentify残数0を返したらHello後もIdentifyしない。
	var limit_fake := preload("res://fake_server.gd").new()
	root.add_child(limit_fake)
	limit_fake.session_remaining = 0
	var limit_ports := limit_fake.start()
	var limit_bot = api.call("bot", "limit-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % limit_ports.rest,
	})
	check(limit_bot != null and limit_bot.call("start") == OK, "identify limit bot start")
	check(await wait_for(func(): return limit_fake.hello_count == 1) and limit_fake.identify_count == 0, "identify limit stopped")
	limit_bot.call("close")
	limit_fake.queue_free()
	await process_frame
	await process_frame

	# 同tokenの2 clientはGateway Bot APIの残数を共有する。
	var shared_fake := preload("res://fake_server.gd").new()
	root.add_child(shared_fake)
	shared_fake.session_remaining = 1
	var shared_ports := shared_fake.start()
	var shared_a = api.call("bot", "shared-token", {"api_url": "http://127.0.0.1:%d/api/v10" % shared_ports.rest})
	var shared_b = api.call("bot", "shared-token", {"api_url": "http://127.0.0.1:%d/api/v10" % shared_ports.rest})
	check(shared_a.call("start") == OK and shared_b.call("start") == OK, "shared identify clients start")
	check(await wait_for(func(): return shared_fake.hello_count >= 2 and shared_fake.identify_count == 1), "shared identify budget")
	var identify_until := Time.get_ticks_msec() + 5200
	while Time.get_ticks_msec() < identify_until:
		await process_frame
	check(shared_fake.identify_count == 1, "shared identify budget retained")
	shared_a.call("close")
	shared_b.call("close")
	shared_fake.queue_free()
	await process_frame
	await process_frame

	# rolling上限はclientを作り直してもSQLiteに残る。
	var stored_fake := preload("res://fake_server.gd").new()
	root.add_child(stored_fake)
	var stored_ports := stored_fake.start()
	var stored_a = api.call("bot", "stored-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % stored_ports.rest,
		"identify_limit": 1,
	})
	check(stored_a.call("start") == OK, "stored identify first start")
	check(await wait_for(func(): return stored_fake.identify_count == 1), "stored identify first use")
	stored_a.call("close")
	var stored_b = api.call("bot", "stored-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % stored_ports.rest,
		"identify_limit": 1,
	})
	check(stored_b.call("start") == OK, "stored identify second start")
	check(await wait_for(func(): return stored_fake.hello_count >= 2), "stored identify second hello")
	var stored_until := Time.get_ticks_msec() + 5200
	while Time.get_ticks_msec() < stored_until:
		await process_frame
	check(stored_fake.identify_count == 1, "stored identify persisted")
	stored_b.call("close")
	stored_fake.queue_free()
	await process_frame
	await process_frame

	# 公式枠の期限後はローカル復旧せずGateway Bot APIを再取得する。
	var reset_fake := preload("res://fake_server.gd").new()
	root.add_child(reset_fake)
	reset_fake.session_remaining = 1
	reset_fake.session_reset_ms = 100
	var reset_ports := reset_fake.start()
	var reset_bot = api.call("bot", "reset-token", {"api_url": "http://127.0.0.1:%d/api/v10" % reset_ports.rest})
	check(reset_bot.call("start") == OK, "identify reset bot start")
	check(await wait_for(func(): return reset_fake.identify_count >= 2), "identify after official refresh")
	check(reset_fake.gateway_bot_count >= 2, "official identify budget refreshed")
	reset_bot.call("close")
	reset_fake.queue_free()
	await process_frame
	await process_frame

	var release_fake := preload("res://fake_server.gd").new()
	root.add_child(release_fake)
	var release_ports := release_fake.start()
	release_bot = api.call("bot", "release-token", {
		"api_url": "http://127.0.0.1:%d/api/v10" % release_ports.rest,
	})
	release_bot.ready.connect(_release_ready)
	check(release_bot.call("start") == OK, "release bot start")
	check(await wait_for(func(): return release_bot == null), "release during ready")
	release_fake.queue_free()
	await process_frame
	await process_frame
	api = null
	var unloaded := GDExtensionManager.unload_extension(EXT)
	check(unloaded == GDExtensionManager.LOAD_STATUS_OK, "unload extension")
	check(not Engine.has_singleton("GDDiscord"), "singleton removed")
	print("checks=%d failures=%d" % [checks, failures])
	quit(failures)
