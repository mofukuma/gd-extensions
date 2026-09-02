# Discord Gateway v10とRESTの必要部分だけをlocalhostで再現する試験server。
extends Node

var rest := TCPServer.new() # REST要求を受けるserver
var gateway := TCPServer.new() # WebSocket Gatewayを受けるserver
var rest_peers: Array[Dictionary] = [] # 読取り途中のREST接続
var sockets: Array[Dictionary] = [] # Gateway WebSocketと接続状態
var rest_count := 0 # message作成を受けた回数
var global_at := 0 # global 429を返した時刻
var rest_global_at: Array[int] = [] # global件数試験を受けた時刻
var gateway_bot_count := 0 # Gateway Bot情報を返した回数
var identify_token := "" # Identifyで受けたtoken
var identify_count := 0 # 新規Identifyを受けた回数
var resume_count := 0 # Resumeを受けた回数
var presence_count := 0 # presence更新を受けた回数
var gateway_url := "" # Readyへ返す再開URL
var close_code := -1 # clientから受けたclose code
var limited_count := 0 # 常に429のendpointが呼ばれた回数
var limited_at := 0 # 最後の429を返した時刻
var rest_retry := 0.05 # 常に429のendpointが返す待機秒
var after_at := 0 # 429後の要求を受けた時刻
var bucket_at := 0 # 共有bucketを消費した時刻
var bucket_use_at := 0 # 共有bucketの別routeを受けた時刻
var forbidden_count := 0 # invalid応答試験を受けた回数
var unauthorized_count := 0 # 401停止試験を受けた回数
var pending_count := 0 # 未応答予約の解除試験を受けた回数
var slow_count := 0 # 低FPS試験を受けた回数
var session_remaining := 999 # Gateway Bot APIが返すIdentify残数
var session_total := 1000 # Gateway Bot APIが返すIdentify総数
var session_reset_ms := 86400000 # Gateway Bot APIが返す残り時間ms
var hello_count := 0 # Gateway Helloを送った回数


# localhostの空きportでRESTとGatewayを始める。
func start() -> Dictionary:
	if rest.listen(0, "127.0.0.1") != OK:
		return {}
	if gateway.listen(0, "127.0.0.1") != OK:
		return {}
	gateway_url = "ws://127.0.0.1:%d" % gateway.get_local_port()
	return {"rest": rest.get_local_port(), "gateway": gateway.get_local_port()}


# 新規接続を取り込み、RESTとGatewayを少しずつ進める。
func _process(_delta):
	while rest.is_connection_available():
		rest_peers.append({"peer": rest.take_connection(), "bytes": PackedByteArray()})
	while gateway.is_connection_available():
		var ws := WebSocketPeer.new()
		if ws.accept_stream(gateway.take_connection()) != OK:
			continue
		sockets.append({"ws": ws, "hello": false, "close_at": 0})
	_process_rest()
	_process_gateway()


# 完成したREST要求へDiscord風JSONを返す。
func _process_rest():
	for i in range(rest_peers.size() - 1, -1, -1):
		var item := rest_peers[i]
		var peer: StreamPeerTCP = item.peer
		if peer.poll() != OK:
			rest_peers.remove_at(i)
			continue
		var count := peer.get_available_bytes()
		if count > 0:
			var got := peer.get_data(count)
			if got[0] == OK:
				item.bytes.append_array(got[1])
		if _http_complete(item.bytes):
			_respond_rest(peer, item.bytes.get_string_from_utf8())
			rest_peers.remove_at(i)


# headerと宣言済みbodyが揃ったか判断する。
func _http_complete(bytes: PackedByteArray) -> bool:
	var text := bytes.get_string_from_utf8()
	var split := text.find("\r\n\r\n")
	if split < 0:
		return false
	var length := 0
	for line in text.left(split).split("\r\n"):
		if line.to_lower().begins_with("content-length:"):
			length = line.get_slice(":", 1).strip_edges().to_int()
	return bytes.size() >= split + 4 + length


# REST要求をmethod、path、header、JSON bodyへ分ける。
func _http_request(text: String) -> Dictionary:
	var split := text.find("\r\n\r\n")
	var lines := text.left(split).split("\r\n")
	var first := lines[0].split(" ")
	var headers := {}
	for line in lines.slice(1):
		var at := line.find(":")
		if at > 0:
			headers[line.left(at).to_lower()] = line.substr(at + 1).strip_edges()
	var raw_body := text.substr(split + 4)
	return {
		"method": first[0],
		"path": first[1],
		"headers": headers,
		"body": null if raw_body.is_empty() else JSON.parse_string(raw_body),
	}


# 初回だけ429、再試行にはmessageを返してrate limit処理を確かめる。
func _respond_rest(peer: StreamPeerTCP, text: String):
	var req := _http_request(text)
	var status := 200
	var data
	var extra := "X-RateLimit-Remaining: 1\r\nX-RateLimit-Reset-After: 0.05\r\n"
	if req.method == "GET" and req.path == "/api/v10/gateway/bot":
		gateway_bot_count += 1
		data = {
			"url": gateway_url,
			"shards": 1,
			"session_start_limit": {
				"total": session_total,
				"remaining": session_remaining,
				"reset_after": session_reset_ms,
				"max_concurrency": 1,
			},
		}
	elif req.method == "POST" and req.path == "/api/v10/channels/123/messages":
		rest_count += 1
		if rest_count == 1:
			global_at = Time.get_ticks_msec()
			status = 429
			data = {"message": "rate limited"}
			extra = "Retry-After: 0.05\r\nX-RateLimit-Global: true\r\nX-RateLimit-Remaining: 0\r\n"
		else:
			data = {
				"id": "456",
				"content": req.body.content,
				"mentions": req.body.get("allowed_mentions", {}),
				"auth": req.headers.get("authorization", ""),
			}
	elif req.path == "/api/v10/always429":
		limited_count += 1
		limited_at = Time.get_ticks_msec()
		status = 429
		data = {"message": "still limited", "retry_after": rest_retry, "global": true}
		extra = "Retry-After: %s\r\nX-RateLimit-Global: true\r\n" % rest_retry
	elif req.path == "/api/v10/after429":
		after_at = Time.get_ticks_msec()
		data = {"after": true}
	elif req.path.begins_with("/api/v10/global/"):
		rest_global_at.append(Time.get_ticks_msec())
		data = {"global": true}
	elif req.path == "/api/v10/bucket/seed":
		data = {"seed": true}
		extra = "X-RateLimit-Bucket: shared-test\r\nX-RateLimit-Remaining: 1\r\n"
	elif req.path == "/api/v10/bucket/exhaust":
		bucket_at = Time.get_ticks_msec()
		data = {"exhaust": true}
		extra = "X-RateLimit-Bucket: shared-test\r\nX-RateLimit-Remaining: 0\r\nX-RateLimit-Reset-After: 0.05\r\n"
	elif req.path == "/api/v10/bucket/use":
		bucket_use_at = Time.get_ticks_msec()
		data = {"use": true}
		extra = "X-RateLimit-Bucket: shared-test\r\nX-RateLimit-Remaining: 1\r\n"
	elif req.path == "/api/v10/forbidden":
		forbidden_count += 1
		status = 403
		data = {"message": "forbidden"}
	elif req.path == "/api/v10/unauthorized":
		unauthorized_count += 1
		status = 401
		data = {"message": "unauthorized"}
	elif req.path == "/api/v10/pending":
		pending_count += 1
		data = {"pending": true}
	elif req.path == "/api/v10/slow-frame":
		slow_count += 1
		data = {"slow": true}
	elif req.method == "DELETE":
		status = 204
		data = null
	else:
		status = 404
		data = {"message": "not found"}
	var body := "" if data == null else JSON.stringify(data)
	var reason := "No Content" if status == 204 else ("Too Many Requests" if status == 429 else ("Forbidden" if status == 403 else ("Unauthorized" if status == 401 else "OK")))
	var response := "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n%sContent-Length: %d\r\nConnection: close\r\n\r\n%s" % [status, reason, extra, body.to_utf8_buffer().size(), body]
	var _sent := peer.put_data(response.to_utf8_buffer())
	peer.disconnect_from_host()


# Gateway handshake、Identify、Resume、heartbeat、dispatchを再現する。
func _process_gateway():
	for i in range(sockets.size() - 1, -1, -1):
		var item := sockets[i]
		var ws: WebSocketPeer = item.ws
		ws.poll()
		if ws.get_ready_state() == WebSocketPeer.STATE_CLOSED:
			if ws.get_close_code() == 1000:
				close_code = 1000
			sockets.remove_at(i)
			continue
		if ws.get_ready_state() != WebSocketPeer.STATE_OPEN:
			continue
		if item.close_at > 0 and Time.get_ticks_msec() >= item.close_at:
			item.close_at = 0
			ws.close(4007, "invalid sequence")
			continue
		if not item.hello:
			item.hello = true
			hello_count += 1
			_send(ws, {"op": 10, "d": {"heartbeat_interval": 1000}})
		while ws.get_available_packet_count() > 0:
			var payload = JSON.parse_string(ws.get_packet().get_string_from_utf8())
			if payload.op == 2:
				identify_token = payload.d.token
				identify_count += 1
				_send(ws, {"op": 0, "s": 1, "t": "READY", "d": {"session_id": "session-test", "resume_gateway_url": gateway_url, "user": {"id": "bot-1"}}})
				if identify_count == 1:
					_send(ws, {"op": 0, "s": 2, "t": "MESSAGE_CREATE", "d": {"id": "m1", "content": "hello"}})
					_send(ws, {"op": 7, "d": null})
			elif payload.op == 6:
				resume_count += 1
				_send(ws, {"op": 0, "s": 3, "t": "RESUMED", "d": {}})
				item.close_at = Time.get_ticks_msec() + 100
			elif payload.op == 1:
				_send(ws, {"op": 11, "d": null})
			elif payload.op == 3:
				presence_count += 1


# JSONをGateway text frameで送る。
func _send(ws: WebSocketPeer, payload: Dictionary):
	var _sent := ws.send_text(JSON.stringify(payload))
