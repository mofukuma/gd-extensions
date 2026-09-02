# Supabase REST/Authの必要部分だけをlocalhostで再現する試験server。
extends Node

var server := TCPServer.new() # 空きportで要求を受けるserver
var peers: Array[Dictionary] = [] # 読取り途中の接続と受信bytes


# localhostの空きportで待受けを始める。
func start() -> int:
	var e := server.listen(0, "127.0.0.1")
	if e != OK:
		return -1
	return server.get_local_port()


# 新規接続を取り込み、完成したHTTP要求へ応答する。
func _process(_delta):
	while server.is_connection_available():
		peers.append({"peer": server.take_connection(), "bytes": PackedByteArray()})

	for i in range(peers.size() - 1, -1, -1):
		var item := peers[i]
		var peer: StreamPeerTCP = item.peer
		peer.poll()
		var count := peer.get_available_bytes()
		if count > 0:
			var got := peer.get_data(count)
			if got[0] == OK:
				item.bytes.append_array(got[1])
		if _complete(item.bytes):
			_respond(peer, item.bytes.get_string_from_utf8())
			peers.remove_at(i)


# headerと宣言済みbodyが揃ったか判断する。
func _complete(bytes: PackedByteArray) -> bool:
	var text := bytes.get_string_from_utf8()
	var split := text.find("\r\n\r\n")
	if split < 0:
		return false
	var length := 0
	for line in text.left(split).split("\r\n"):
		if line.to_lower().begins_with("content-length:"):
			length = line.get_slice(":", 1).strip_edges().to_int()
	return bytes.size() >= split + 4 + length


# HTTP要求からmethod、path、header、JSON bodyを読む。
func _request(text: String) -> Dictionary:
	var split := text.find("\r\n\r\n")
	var lines := text.left(split).split("\r\n")
	var first := lines[0].split(" ")
	var headers := {}
	for line in lines.slice(1):
		var at := line.find(":")
		if at > 0:
			headers[line.left(at).to_lower()] = line.substr(at + 1).strip_edges()
	var raw_body := text.substr(split + 4)
	var body = null if raw_body.is_empty() else JSON.parse_string(raw_body)
	return {
		"method": first[0],
		"path": first[1],
		"headers": headers,
		"body": body,
	}


# routeに応じたSupabase互換JSONを返す。
func _respond(peer: StreamPeerTCP, text: String):
	var req := _request(text)
	var status := 200
	var data
	if req.path.begins_with("/auth/v1/token?grant_type=password"):
		data = {"access_token": "access-test", "refresh_token": "refresh-test", "user": {"id": "user-1"}}
	elif req.path == "/auth/v1/settings":
		data = {"disable_signup": false, "mailer_autoconfirm": true}
	elif req.path.begins_with("/auth/v1/token?grant_type=refresh_token"):
		data = {"access_token": "access-refreshed", "refresh_token": "refresh-test", "user": {"id": "user-1"}}
	elif req.path == "/auth/v1/logout":
		status = 204
		data = null
	elif req.path.begins_with("/rest/v1/rpc/add"):
		data = {"sum": req.body.a + req.body.b}
	elif req.path.begins_with("/rest/v1/missing"):
		status = 404
		data = {"message": "not found"}
	elif req.method == "GET":
		data = [{
			"id": 1,
			"apikey": req.headers.get("apikey", ""),
			"auth": req.headers.get("authorization", ""),
			"path": req.path,
		}]
	else:
		data = [{"method": req.method, "body": req.body, "path": req.path}]
	var body := "" if data == null else JSON.stringify(data)
	var reason := "No Content" if status == 204 else "OK"
	var response := "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s" % [status, reason, body.to_utf8_buffer().size(), body]
	peer.put_data(response.to_utf8_buffer())
	peer.disconnect_from_host()
