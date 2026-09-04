# Discord Gateway v10とRESTをC++なしで扱う純GDScript module。
extends RefCounted

const GUILDS := 1 # guild関連eventを受けるintent
const GUILD_MEMBERS := 2 # guild member eventを受けるintent
const GUILD_MODERATION := 4 # moderation eventを受けるintent
const GUILD_PRESENCES := 256 # presence eventを受けるintent
const GUILD_MESSAGES := 512 # guild message eventを受けるintent
const GUILD_MESSAGE_REACTIONS := 1024 # guild reaction eventを受けるintent
const DIRECT_MESSAGES := 4096 # direct message eventを受けるintent
const DIRECT_MESSAGE_REACTIONS := 8192 # direct reaction eventを受けるintent
const MESSAGE_CONTENT := 32768 # message本文を受けるintent
const API := "https://discord.com/api/v10" # Discord RESTの基準URL
const PACKET_MIN := 64 * 1024 # Gateway packet設定の下限bytes
const PACKET_MAX := 8 * 1024 * 1024 # Gateway packet設定の上限bytes
const PAYLOAD_MAX := 4096 # Discordへ送るGateway payloadの上限bytes
const BODY_MAX := 8 * 1024 * 1024 # RESTへ送るJSONの上限bytes
const RETRY_MAX := 4 # 429を再試行する回数

# REST完了signalと要求情報を保持する箱。
class Call extends RefCounted:
	@warning_ignore("unused_signal")
	signal finished(reply)

	var method # HTTP method
	var path := "" # APIからの相対path
	var route := "" # rate limit用に正規化したroute
	var major := "" # bucketを分けるchannel、guild、webhook
	var body := "" # JSONへ変換済みの送信body
	var bytes := 0 # queue予算へ数えるbody bytes
	var retries := 0 # 実施済み429再試行回数


# Clientの通信をSceneTreeのframeで駆動する内部Node。
class Pump extends Node:
	var client # 駆動中のDiscord clientへの弱い参照

	# clientの通信を1frame進める。
	func _process(delta):
		var value = client.get_ref() if client != null else null
		if value == null:
			queue_free()
		else:
			value._tick(delta)


# 一つのBot接続とREST queueをframeごとに進めるclient。
class Client extends RefCounted:
	signal ready(data)
	signal event(name, data)
	signal resumed
	signal disconnected(code, reason)
	signal failed(message)

	static var global_until := {} # token別global rate limit解除時刻
	static var route_bucket := {} # routeからDiscord bucketへの対応
	static var bucket_until := {} # tokenとbucket別の解除時刻
	static var bucket_seen := {} # bucket情報を24時間で掃除する最終利用時刻
	static var bucket_prune_at := 0 # 次回bucket掃除時刻
	static var sent := {} # token別の直近REST送信時刻
	static var invalid := [] # process内で共有するinvalid応答時刻
	static var identify := {} # token別のIdentify残数とreset時刻
	static var identify_next := {} # token別の次回Identify可能時刻

	var token := "" # Discord Bot token
	var key := "" # tokenを露出しない共有制限key
	var api_url := API # RESTの基準URL
	var intents := 0 # Gatewayへ要求するintent bits
	var max_packet := 2 * 1024 * 1024 # 1frameで受けるGateway bytes
	var max_response := 8 * 1024 * 1024 # REST response上限bytes
	var max_queue := 256 # 待てるREST件数
	var max_queue_bytes := 16 * 1024 * 1024 # 待てるREST body総量
	var invalid_limit := 900 # 10分内のinvalid応答上限
	var identify_limit := 900 # 24時間内の安全側Identify上限
	var queue := [] # REST要求のFIFO
	var queue_bytes := 0 # 待ちREST body総量
	var active # 実行中のREST要求
	var http # 実行中のREST HTTPRequest
	var gateway_http # Gateway URL取得用HTTPRequest
	var socket # Gateway WebSocketPeer
	var gateway_pending := PackedByteArray() # 次frameへ送るGateway packet
	var gateway_pending_text := false # 待ちpacketがtextか
	var started := false # Gateway接続を続けるか
	var authorized := true # 401後の送信を止める状態
	var closing := false # close処理中か
	var close_deadline := 0 # close handshakeを待つ上限時刻
	var gateway_url := "" # Gateway Bot APIから得た接続先
	var resume_url := "" # READYから得た再接続先
	var session_id := "" # Resumeへ使うsession ID
	var sequence := -1 # 最後に受けたGateway sequence
	var resume_next := false # 次回接続でResumeするか
	var reconnect_at := 0 # 次回Gateway接続時刻
	var reconnect_count := 0 # backoff段数
	var hello := false # Hello受信済みか
	var identify_pending := false # Hello後のIdentify送信待ちか
	var ready_state := false # READYまたはRESUMED済みか
	var heartbeat_ms := 0 # Discord指定heartbeat間隔
	var heartbeat_at := 0 # 次のheartbeat時刻
	var heartbeat_sent := 0 # latency測定用送信時刻
	var heartbeat_ack := true # 最後のheartbeatへACK済みか
	var latency_ms := -1 # 最終heartbeat latency
	var presence # 送信待ちpresence
	var presence_at := [] # 過去20秒のpresence送信時刻
	var pump # SceneTreeへ接続する駆動Node

	# tokenとoptionを検証してclientを初期化する。
	func setup(bot_token, options):
		if bot_token.is_empty() or bot_token.length() > 4096 or "\r" in bot_token or "\n" in bot_token \
			or not options is Dictionary or options.has("gateway_url"):
			return false
		token = bot_token
		key = bot_token.sha256_text()
		api_url = str(options.get("api_url", API)).trim_suffix("/")
		intents = int(options.get("intents", 0))
		max_packet = int(options.get("max_gateway_packet", max_packet))
		max_response = int(options.get("max_response", max_response))
		max_queue = int(options.get("max_rest_queue", max_queue))
		max_queue_bytes = int(options.get("max_queue_bytes", max_queue_bytes))
		invalid_limit = int(options.get("invalid_limit", invalid_limit))
		identify_limit = int(options.get("identify_limit", identify_limit))
		var ok = _api_ok(api_url) and max_packet >= PACKET_MIN and max_packet <= PACKET_MAX \
			and intents >= 0 and max_response >= 1024 and max_response <= 64 * 1024 * 1024 \
			and max_queue > 0 and max_queue <= 1024 \
			and max_queue_bytes >= 1024 and max_queue_bytes <= 64 * 1024 * 1024 \
			and invalid_limit > 0 and invalid_limit <= 900 \
			and identify_limit > 0 and identify_limit <= 1000000
		if ok:
			pump = Pump.new()
			pump.client = weakref(self)
		return ok

	# Gateway URLを取得して接続を始める。
	func start():
		if started or closing or not authorized:
			return ERR_ALREADY_IN_USE
		started = true
		_load_gateway()
		return OK

	# Gatewayと未完了REST要求を閉じる。
	func close(code = 1000, reason = ""):
		if closing:
			return
		closing = true
		started = false
		var wait_socket = socket != null and socket.get_ready_state() != WebSocketPeer.STATE_CLOSED
		if wait_socket:
			socket.close(code, reason)
			close_deadline = Time.get_ticks_msec() + 1000
		for req in queue:
			_finish(req, _reply(false, 0, null, "client closed"))
		queue.clear()
		queue_bytes = 0
		if active != null:
			_finish(active, _reply(false, 0, null, "client closed"))
			active = null
		if http != null:
			http.queue_free()
			http = null
		if gateway_http != null:
			gateway_http.queue_free()
			gateway_http = null
		if not wait_socket:
			_finish_close()

	# Botのpresenceを次の安全な送信機会へまとめる。
	func set_presence(data):
		if not data is Dictionary or not data.has_all(["since", "activities", "status", "afk"]):
			return ERR_INVALID_PARAMETER
		if data.since != null and not data.since is int:
			return ERR_INVALID_PARAMETER
		if not data.activities is Array or not data.afk is bool:
			return ERR_INVALID_PARAMETER
		if not str(data.status) in ["online", "dnd", "idle", "invisible", "offline"]:
			return ERR_INVALID_PARAMETER
		if JSON.stringify({"op": 3, "d": data}).to_utf8_buffer().size() > PAYLOAD_MAX:
			return ERR_OUT_OF_MEMORY
		presence = data
		return OK

	# 任意のDiscord REST endpointをqueueへ入れ、完了signalを返す。
	func request(method_name, path, body = null):
		var methods := {
			"GET": HTTPClient.METHOD_GET,
			"POST": HTTPClient.METHOD_POST,
			"PUT": HTTPClient.METHOD_PUT,
			"PATCH": HTTPClient.METHOD_PATCH,
			"DELETE": HTTPClient.METHOD_DELETE,
		}
		var req := Call.new()
		var name := str(method_name).to_upper()
		if not methods.has(name):
			call_deferred("_finish", req, _reply(false, 0, null, "unsupported HTTP method"))
			return req.finished
		req.method = methods[name]
		req.path = str(path)
		req.route = _route(name, req.path)
		req.major = _major(req.path)
		req.body = "" if body == null else JSON.stringify(body)
		req.bytes = req.body.to_utf8_buffer().size()
		var path_ok := req.path.begins_with("/") and not req.path.begins_with("//") \
			and req.path.length() <= 2048 and not "://" in req.path \
			and not "\r" in req.path and not "\n" in req.path
		if not path_ok or req.bytes > BODY_MAX:
			call_deferred("_finish", req, _reply(false, 0, null, "invalid REST request"))
		elif closing:
			call_deferred("_finish", req, _reply(false, 0, null, "client closed"))
		elif not authorized or queue.size() + (1 if active != null else 0) >= max_queue \
			or req.bytes > max_queue_bytes - queue_bytes:
			call_deferred("_finish", req, _reply(false, 0, null, "REST queue is full"))
		else:
			queue.append(req)
			queue_bytes += req.bytes
		return req.finished

	# channelへmessageを送る。
	func send_message(channel_id, message):
		if not _snowflake(channel_id):
			return request("POST", "", null)
		return request("POST", "/channels/%s/messages" % channel_id, _message(message))

	# 既存messageを更新する。
	func edit_message(channel_id, message_id, message):
		if not _snowflake(channel_id) or not _snowflake(message_id):
			return request("PATCH", "", null)
		return request("PATCH", "/channels/%s/messages/%s" % [channel_id, message_id], _message(message))

	# 既存messageを削除する。
	func delete_message(channel_id, message_id):
		if not _snowflake(channel_id) or not _snowflake(message_id):
			return request("DELETE", "", null)
		return request("DELETE", "/channels/%s/messages/%s" % [channel_id, message_id])

	# READY済みか返す。
	func is_ready():
		return started and ready_state

	# 最終heartbeat latencyを返す。
	func get_latency_ms():
		return latency_ms

	# 待ちREST要求数を返す。
	func pending():
		return queue.size() + (1 if active != null else 0)

	# 現Gateway session IDを返す。
	func get_session_id():
		return session_id

	# GatewayとRESTをframeごとに少しずつ進める。
	func _tick(_delta):
		if closing:
			if socket != null:
				socket.poll()
				if socket.get_ready_state() != WebSocketPeer.STATE_CLOSED and Time.get_ticks_msec() < close_deadline:
					return
			_finish_close()
			return
		var now := Time.get_ticks_msec()
		_process_gateway(now)
		_process_presence(now)
		_start_rest(now)

	# Gateway Bot APIから安全なWebSocket URLを得る。
	func _load_gateway():
		if gateway_http != null:
			return
		gateway_http = HTTPRequest.new()
		gateway_http.timeout = 30.0
		gateway_http.body_size_limit = 1024 * 1024
		gateway_http.max_redirects = 0
		pump.add_child(gateway_http)
		gateway_http.request_completed.connect(_gateway_info, CONNECT_ONE_SHOT)
		var headers := PackedStringArray(["Authorization: Bot " + token, "Accept: application/json"])
		if gateway_http.request(api_url + "/gateway/bot", headers) != OK:
			_failed("gateway information request could not start")

	# Gateway Bot APIの応答を検証して接続する。
	func _gateway_info(result, status, _raw_headers, body):
		gateway_http.queue_free()
		gateway_http = null
		var data = JSON.parse_string(body.get_string_from_utf8()) if not body.is_empty() else null
		if result != HTTPRequest.RESULT_SUCCESS or status != 200 or not data is Dictionary:
			_failed("gateway information request failed")
			return
		gateway_url = str(data.get("url", ""))
		if not _gateway_ok(gateway_url):
			_failed("gateway URL was rejected")
			return
		var limit = data.get("session_start_limit", {})
		var now := Time.get_ticks_msec()
		var total := int(limit.get("total", 0)) if limit is Dictionary else 0
		var remaining := int(limit.get("remaining", -1)) if limit is Dictionary else -1
		var reset := int(limit.get("reset_after", 0)) if limit is Dictionary else 0
		var concurrency := int(limit.get("max_concurrency", 0)) if limit is Dictionary else 0
		if total < 2 or remaining < 0 or remaining > total or reset < 1 \
			or reset > 7 * 24 * 60 * 60 * 1000 or concurrency < 1:
			_failed("gateway information is invalid")
			return
		var fresh := {
			"remaining": mini(remaining, identify_limit),
			"reset": now + reset,
		}
		if not identify.has(key) or now >= int(identify[key].reset):
			identify[key] = fresh
		else:
			identify[key].remaining = mini(int(identify[key].remaining), int(fresh.remaining))
		_connect_gateway()

	# WebSocketPeerを作りGatewayへ接続する。
	func _connect_gateway():
		if not started:
			return
		var target := resume_url if resume_next and not resume_url.is_empty() else gateway_url
		socket = WebSocketPeer.new()
		socket.inbound_buffer_size = max_packet
		socket.max_queued_packets = 64
		hello = false
		identify_pending = false
		ready_state = false
		heartbeat_ack = true
		var endpoint = _gateway_endpoint(target)
		if socket.connect_to_url(endpoint) != OK:
			_schedule_reconnect(resume_next)

	# Gateway socketをpollし、packetと再接続を進める。
	func _process_gateway(now):
		if not started:
			return
		if socket == null:
			if reconnect_at > 0 and now >= reconnect_at:
				reconnect_at = 0
				_connect_gateway()
			return
		socket.poll()
		var state = socket.get_ready_state()
		if state == WebSocketPeer.STATE_CLOSED:
			var code = socket.get_close_code()
			var reason = socket.get_close_reason()
			var before_hello := not hello
			socket = null
			gateway_pending = PackedByteArray()
			gateway_pending_text = false
			ready_state = false
			disconnected.emit(code, reason)
			if _fatal(code):
				_failed("gateway closed: %d" % code)
			else:
				var can_resume = not session_id.is_empty() and code not in [4007, 4009]
				if can_resume and before_hello:
					resume_url = ""
				if not can_resume:
					session_id = ""
					resume_url = ""
					sequence = -1
				_schedule_reconnect(can_resume)
			return
		if state != WebSocketPeer.STATE_OPEN:
			return
		var used := 0
		for _i in 64:
			var packet := PackedByteArray()
			var text_packet := false
			if not gateway_pending.is_empty():
				packet = gateway_pending
				text_packet = gateway_pending_text
				gateway_pending = PackedByteArray()
				gateway_pending_text = false
			elif socket.get_available_packet_count() > 0:
				packet = socket.get_packet()
				text_packet = socket.was_string_packet()
			else:
				break
			if not text_packet or packet.size() > max_packet:
				_failed("gateway sent an unsupported packet")
				return
			if used > 0 and used + packet.size() > max_packet:
				gateway_pending = packet
				gateway_pending_text = text_packet
				break
			used += packet.size()
			var payload = JSON.parse_string(packet.get_string_from_utf8())
			if not payload is Dictionary:
				_failed("gateway sent invalid JSON")
				return
			_receive(payload, now)
			if socket == null:
				break
		if hello and now >= heartbeat_at:
			if not heartbeat_ack:
				_drop(4000, "heartbeat timeout")
				_schedule_reconnect(true)
				return
			_heartbeat(now)
		if identify_pending:
			_identify(now)

	# 一つのGateway payloadを状態とsignalへ反映する。
	func _receive(payload, now):
		var op := int(payload.get("op", -1))
		if payload.get("s") is int:
			sequence = payload.s
		if op == 0:
			var name := str(payload.get("t", ""))
			var data = payload.get("d")
			if name == "READY" and data is Dictionary:
				session_id = str(data.get("session_id", ""))
				var candidate := str(data.get("resume_gateway_url", ""))
				resume_url = candidate if _gateway_ok(candidate) else ""
				resume_next = true
				ready_state = true
				reconnect_count = 0
				ready.emit(data)
			elif name == "RESUMED":
				resume_next = true
				ready_state = true
				reconnect_count = 0
				resumed.emit()
			event.emit(name, data)
		elif op == 1:
			_heartbeat(now)
		elif op == 7:
			_drop(4000, "reconnect")
			_schedule_reconnect(true, 0)
		elif op == 9:
			var can_resume := bool(payload.get("d")) and not session_id.is_empty()
			if not can_resume:
				session_id = ""
				resume_url = ""
				sequence = -1
			_drop(4000, "invalid session")
			_schedule_reconnect(can_resume, 1000 + now % 4000)
		elif op == 10:
			var data = payload.get("d", {})
			heartbeat_ms = int(data.get("heartbeat_interval", 0)) if data is Dictionary else 0
			if heartbeat_ms < 1000 or heartbeat_ms > 300000:
				_failed("gateway hello is invalid")
				return
			hello = true
			identify_pending = true
			heartbeat_at = now + 1 + now % heartbeat_ms
			_identify(now)
		elif op == 11:
			heartbeat_ack = true
			latency_ms = now - heartbeat_sent if heartbeat_sent > 0 else -1

	# heartbeatを送りACKと次回時刻を追跡する。
	func _heartbeat(now):
		var seq_data = null
		if sequence >= 0:
			seq_data = sequence
		if _send(1, seq_data) == OK:
			heartbeat_ack = false
			heartbeat_sent = now
			heartbeat_at = now + heartbeat_ms

	# Hello後にIdentifyまたはResumeを送る。
	func _identify(now):
		if resume_next and not session_id.is_empty():
			var seq_data = null
			if sequence >= 0:
				seq_data = sequence
			if _send(6, {"token": token, "session_id": session_id, "seq": seq_data}) == OK:
				identify_pending = false
			return
		if now < int(identify_next.get(key, 0)):
			return
		var budget = identify.get(key, {"remaining": identify_limit, "reset": now + 86400000})
		if now >= int(budget.reset):
			_drop(4000, "identify budget refresh")
			_load_gateway()
			return
		if int(budget.remaining) <= 0:
			failed.emit("gateway identify limit is exhausted")
			_drop(4000, "identify limit")
			reconnect_at = int(budget.reset)
			return
		budget.remaining = int(budget.remaining) - 1
		identify[key] = budget
		if _send(2, {
			"token": token,
			"intents": intents,
			"properties": {"os": "godot", "browser": "gd", "device": "gd"},
		}) == OK:
			identify_next[key] = now + 5000
			identify_pending = false

	# 最新presenceをDiscordの5回/20秒枠で送る。
	func _process_presence(now):
		if presence == null or not ready_state or socket == null or socket.get_ready_state() != WebSocketPeer.STATE_OPEN:
			return
		while not presence_at.is_empty() and now - int(presence_at[0]) >= 20000:
			presence_at.pop_front()
		if presence_at.size() >= 5:
			return
		if _send(3, presence) == OK:
			presence_at.append(now)
			presence = null

	# REST queueの先頭を共有制限の解除後に送る。
	func _start_rest(now):
		if http != null or active != null or queue.is_empty():
			return
		_prune(now)
		var req = queue[0]
		var wait := int(global_until.get(key, 0))
		var bucket = route_bucket.get(key + ":" + req.route, req.route)
		var limit_key = key + ":" + bucket + ":" + req.major
		wait = maxi(wait, int(bucket_until.get(limit_key, 0)))
		var times = sent.get(key, [])
		if times.size() >= 45:
			wait = maxi(wait, int(times[0]) + 1100)
		if invalid.size() >= invalid_limit:
			wait = maxi(wait, int(invalid[0]) + 600000)
		if now < wait:
			return
		queue.pop_front()
		active = req
		http = HTTPRequest.new()
		http.timeout = 30.0
		http.body_size_limit = max_response
		http.max_redirects = 0
		pump.add_child(http)
		http.request_completed.connect(_http_done, CONNECT_ONE_SHOT)
		var headers := PackedStringArray([
			"Authorization: Bot " + token,
			"Accept: application/json",
			"User-Agent: DiscordBot (https://github.com/mofukuma/gd-extensions, 0.2)",
		])
		if not req.body.is_empty():
			var _added := headers.append("Content-Type: application/json")
		times.append(now)
		sent[key] = times
		var err = http.request(api_url + req.path, headers, req.method, req.body)
		if err != OK:
			var failed_call = active
			active = null
			queue_bytes -= failed_call.bytes
			_clear_http()
			_finish(failed_call, _reply(false, 0, null, "request could not start: %d" % err))

	# REST応答をrate limitへ反映して利用側へ返す。
	func _http_done(result, status, headers, body):
		if active == null:
			_clear_http()
			return
		var req = active
		active = null
		var map = _headers(headers)
		var now := Time.get_ticks_msec()
		var data = JSON.parse_string(body.get_string_from_utf8()) if not body.is_empty() else null
		var remaining := str(map.get("x-ratelimit-remaining", "1")).to_int()
		var wait = _wait(str(map.get("x-ratelimit-reset-after", "0")).to_float()) if remaining <= 0 else 0
		var bucket := str(map.get("x-ratelimit-bucket", ""))
		if not bucket.is_empty():
			_remember_route(key + ":" + req.route, bucket, now)
		var scope := str(map.get("x-ratelimit-scope", "")).to_lower()
		if result == HTTPRequest.RESULT_SUCCESS and (status in [401, 403] or status == 429 and scope != "shared"):
			invalid.append(now)
		if result == HTTPRequest.RESULT_SUCCESS and status == 429:
			var retry := str(map.get("retry-after", "0")).to_float()
			if data is Dictionary:
				retry = maxf(retry, float(data.get("retry_after", 0)))
			wait = maxi(wait, _wait(maxf(0.05, retry)))
			var global := str(map.get("x-ratelimit-global", "false")).to_lower() == "true" \
				or (data is Dictionary and bool(data.get("global", false)))
			if global:
				global_until[key] = now + wait
			else:
				var limited = bucket if not bucket.is_empty() else req.route
				_remember_wait(key + ":" + limited + ":" + req.major, now + wait, now)
			if req.retries < RETRY_MAX:
				req.retries += 1
				queue.push_front(req)
				_clear_http()
				return
		elif wait > 0:
			_remember_wait(key + ":" + (bucket if not bucket.is_empty() else req.route) + ":" + req.major, now + wait, now)
		var ok = result == HTTPRequest.RESULT_SUCCESS and status >= 200 and status < 300
		var error := ""
		if not ok:
			error = str(data.get("message", "HTTP %d" % status)) if data is Dictionary else "network error %d" % result
		queue_bytes -= req.bytes
		_clear_http()
		_finish(req, _reply(ok, status, data, error, headers))
		if status == 401:
			authorized = false
			started = false
			_drop(4004, "authorization failed")
			for waiting in queue:
				queue_bytes -= waiting.bytes
				_finish(waiting, _reply(false, 401, null, "authorization failed"))
			queue.clear()

	# 完了signalへ共通replyを流す。
	func _finish(req, reply):
		req.finished.emit(reply)

	# 現HTTPRequestをNode treeから外す。
	func _clear_http():
		if http != null:
			http.queue_free()
		http = null

	# close handshake後に駆動Nodeとの循環参照を外す。
	func _finish_close():
		socket = null
		gateway_pending = PackedByteArray()
		gateway_pending_text = false
		if pump != null:
			pump.client = null
			pump.queue_free()
		pump = null

	# Gatewayへ小さいJSON payloadを送る。
	func _send(op, data):
		if socket == null or socket.get_ready_state() != WebSocketPeer.STATE_OPEN:
			return ERR_UNAVAILABLE
		var text := JSON.stringify({"op": op, "d": data})
		if text.to_utf8_buffer().size() > PAYLOAD_MAX:
			return ERR_OUT_OF_MEMORY
		return socket.send_text(text)

	# 現socketを閉じて参照を外す。
	func _drop(code, reason):
		if socket != null and socket.get_ready_state() != WebSocketPeer.STATE_CLOSED:
			socket.close(code, reason)
		socket = null
		gateway_pending = PackedByteArray()
		gateway_pending_text = false
		hello = false
		identify_pending = false
		ready_state = false

	# Gateway再接続を指数backoff付きで予約する。
	func _schedule_reconnect(resume, delay = -1):
		if not started:
			return
		resume_next = resume and not session_id.is_empty()
		var wait = int(delay)
		if wait < 0:
			wait = mini(120000, 1000 << mini(reconnect_count, 7)) + Time.get_ticks_msec() % 500
		reconnect_count += 1
		reconnect_at = Time.get_ticks_msec() + wait

	# 致命的なDiscord close codeか判断する。
	func _fatal(code):
		return code in [4004, 4010, 4011, 4012, 4013, 4014]

	# 継続不能なGateway失敗を通知する。
	func _failed(message):
		started = false
		failed.emit(message)
		_drop(4000, message)

	# tokenを送ってよいREST URLか判断する。
	func _api_ok(url):
		if url == API:
			return true
		return _local(url, "http://") or _local(url, "https://")

	# tokenを送ってよいGateway URLか判断する。
	func _gateway_ok(url):
		if _local(url, "ws://") or _local(url, "wss://"):
			return true
		if not url.begins_with("wss://"):
			return false
		var host = _host(url, "wss://")
		var authority = url.substr(6).get_slice("/", 0).get_slice("?", 0)
		var port_ok = not ":" in authority or authority.ends_with(":443")
		return port_ok and (host == "gateway.discord.gg" or host.ends_with(".discord.gg"))

	# URLがlocalhostだけを指すか判断する。
	func _local(url, scheme):
		var host = _host(url, scheme)
		return host in ["localhost", "127.0.0.1", "[::1]"]

	# URLから認証情報を許さずhostを取り出す。
	func _host(url, scheme):
		if not url.begins_with(scheme):
			return ""
		var authority = url.substr(scheme.length()).get_slice("/", 0).get_slice("?", 0)
		if authority.is_empty() or "@" in authority:
			return ""
		if authority.begins_with("["):
			var end = authority.find("]")
			return authority.left(end + 1).to_lower() if end > 0 else ""
		return authority.get_slice(":", 0).to_lower()

	# Gateway URLへv10とJSON encodingを補う。
	func _gateway_endpoint(url):
		var out = url
		var scheme = out.find("://")
		var query = out.find("?", scheme + 3)
		var slash = out.find("/", scheme + 3)
		if slash < 0 or query >= 0 and slash > query:
			out = out + "/" if query < 0 else out.insert(query, "/")
		out += ("&" if "?" in out else "?") + "v=10" if not "v=" in out else ""
		out += ("&" if "?" in out else "?") + "encoding=json" if not "encoding=" in out else ""
		return out

	# HTTP response headerを小文字keyのDictionaryへ直す。
	func _headers(headers):
		var out := {}
		for line in headers:
			var at = line.find(":")
			if at > 0:
				out[line.left(at).strip_edges().to_lower()] = line.substr(at + 1).strip_edges()
		return out

	# rate limit秒を最大24時間のmsへ安全に直す。
	func _wait(seconds):
		if is_nan(seconds) or is_inf(seconds) or seconds <= 0:
			return 0
		return int(minf(seconds, 86400.0) * 1000.0) + 50

	# 古いprocess内共有時刻を捨てる。
	func _prune(now):
		var times = sent.get(key, [])
		while not times.is_empty() and now - int(times[0]) >= 1100:
			times.pop_front()
		sent[key] = times
		while not invalid.is_empty() and now - int(invalid[0]) >= 600000:
			invalid.pop_front()
		_prune_buckets(now)

	# routeとbucketの共有情報を記録する。
	func _remember_route(route_key, bucket, now):
		route_bucket[route_key] = bucket
		bucket_seen["r:" + route_key] = now
		_prune_buckets(now)

	# bucket解除時刻を記録する。
	func _remember_wait(limit_key, until, now):
		bucket_until[limit_key] = until
		bucket_seen["b:" + limit_key] = now
		_prune_buckets(now)

	# 古いbucket情報を捨て、全processで4096件以内に保つ。
	func _prune_buckets(now):
		if now < bucket_prune_at and bucket_seen.size() <= 4096:
			return
		bucket_prune_at = now + 60000
		for seen_key in bucket_seen.keys():
			if now - int(bucket_seen[seen_key]) >= 86400000:
				_erase_bucket(seen_key)
		if bucket_seen.size() > 4096:
			var keys := bucket_seen.keys()
			keys.sort_custom(func(a, b): return int(bucket_seen[a]) < int(bucket_seen[b]))
			for i in bucket_seen.size() - 4096:
				_erase_bucket(keys[i])

	# 種類prefixに対応する共有情報を一件消す。
	func _erase_bucket(seen_key):
		var _seen := bucket_seen.erase(seen_key)
		if seen_key.begins_with("r:"):
			var _route_removed := route_bucket.erase(seen_key.substr(2))
		else:
			var _limit := bucket_until.erase(seen_key.substr(2))

	# REST pathをDiscord routeへ正規化する。
	func _route(method, path):
		var parts = path.get_slice("?", 0).split("/", false)
		for i in parts.size():
			var major = i > 0 and parts[i - 1] in ["channels", "guilds", "webhooks"] \
				or i > 1 and parts[i - 2] == "webhooks"
			if parts[i].is_valid_int() and not major:
				parts[i] = ":id"
		return method + ":/" + "/".join(parts)

	# Discord bucketを分ける主要resource値をpathから取る。
	func _major(path):
		var parts = path.get_slice("?", 0).split("/", false)
		for i in range(1, parts.size()):
			if parts[i - 1] in ["channels", "guilds"]:
				return parts[i - 1] + ":" + parts[i]
			if parts[i - 1] == "webhooks":
				return "webhooks:" + parts[i] + (":" + parts[i + 1] if i + 1 < parts.size() else "")
		return ""

	# Discord snowflakeとして安全な数字か確かめる。
	func _snowflake(value):
		var text := str(value)
		return not text.is_empty() and text.length() <= 32 and text.is_valid_int() and not text.begins_with("-")

	# String messageへmention無効設定を補う。
	func _message(message):
		if message is Dictionary:
			return message
		return {"content": str(message), "allowed_mentions": {"parse": []}}

	# 公開REST結果の形を揃える。
	func _reply(ok, status, data, error, headers = PackedStringArray()):
		return {"ok": ok, "status": status, "data": data, "error": error, "headers": headers}


# 設定済みBot clientを作ってSceneTreeへ接続する。
static func bot(token, options = {}):
	var tree = Engine.get_main_loop()
	if tree == null or not tree.has_method("get_root"):
		return null
	var client := Client.new()
	if not client.setup(str(token), options):
		return null
	tree.get_root().add_child(client.pump)
	return client
