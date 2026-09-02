# Memcached basic text protocolをlocalhostで再現する試験server。
extends Node

var server := TCPServer.new() # 空きportで要求を受けるserver
var peers: Array[Dictionary] = [] # 接続ごとの受信bytesと分割送信queue
var values := {} # keyごとのbytes、flags、期限
var accepted := 0 # persistent接続の再利用確認用accept数


# localhostの空きportで待受けを始める。
func start() -> int:
	var e := server.listen(0, "127.0.0.1")
	if e != OK:
		return -1
	return server.get_local_port()


# 新規接続、分割送信、command解析を毎frame進める。
func _process(_delta):
	while server.is_connection_available():
		accepted += 1
		peers.append({"peer": server.take_connection(), "bytes": PackedByteArray(), "chunks": []})

	for i in range(peers.size() - 1, -1, -1):
		var item := peers[i]
		var peer: StreamPeerTCP = item.peer
		var _polled := peer.poll()
		if peer.get_status() == StreamPeerTCP.STATUS_NONE or peer.get_status() == StreamPeerTCP.STATUS_ERROR:
			peers.remove_at(i)
			continue
		# 1frameに1片だけ送り、clientの断片応答parserを通す。
		if not item.chunks.is_empty():
			var _sent := peer.put_data(item.chunks.pop_front())
		var count := peer.get_available_bytes()
		if count > 0:
			var got := peer.get_data(count)
			if got[0] == OK:
				item.bytes.append_array(got[1])
		while _consume(item):
			pass


# bytes内のCRLF開始位置を探す。
func _crlf(bytes: PackedByteArray, from := 0) -> int:
	for i in range(from, bytes.size() - 1):
		if bytes[i] == 13 and bytes[i + 1] == 10:
			return i
	return -1


# 期限切れentryをmiss扱いにする。
func _entry(key: String):
	if not values.has(key):
		return null
	var item: Dictionary = values[key]
	if item.expires > 0 and Time.get_ticks_msec() >= item.expires:
		values.erase(key)
		return null
	return item


# 応答を複数片へ割り、送信queueへ積む。
func _reply(item: Dictionary, bytes: PackedByteArray):
	var first: int = mini(3, bytes.size())
	var middle: int = first + (bytes.size() - first) / 2
	if first > 0:
		item.chunks.append(bytes.slice(0, first))
	if middle > first:
		item.chunks.append(bytes.slice(first, middle))
	if bytes.size() > middle:
		item.chunks.append(bytes.slice(middle))


# 指定keyに対応する壊れたGET応答を作る。
func _broken_get(key: String):
	if key.ends_with(":bad_value"):
		return "VALUE broken\r\n".to_utf8_buffer()
	if key.ends_with(":bad_size"):
		return ("VALUE %s 0 20000000\r\n" % key).to_utf8_buffer()
	if key.ends_with(":bad_term"):
		return ("VALUE %s 0 1\r\nxZZ" % key).to_utf8_buffer()
	if key.ends_with(":foreign"):
		return "VALUE another 0 1\r\nx\r\nEND\r\n".to_utf8_buffer()
	if key.ends_with(":bad_flags"):
		return ("VALUE %s 4294967296 1\r\nx\r\nEND\r\n" % key).to_utf8_buffer()
	if key.ends_with("overflow"):
		var out := PackedByteArray()
		out.resize(600)
		out.fill(120)
		return out
	if key.ends_with("cumulative"):
		var out := PackedByteArray()
		var value := PackedByteArray()
		value.resize(200)
		for _i in 4:
			out.append_array(("VALUE %s 0 200\r\n" % key).to_utf8_buffer())
			out.append_array(value)
			out.append_array("\r\n".to_utf8_buffer())
		out.append_array("END\r\n".to_utf8_buffer())
		return out
	return null


# commandを1件だけ消費し、未完成ならfalseを返す。
func _consume(item: Dictionary) -> bool:
	var bytes: PackedByteArray = item.bytes
	var line_end := _crlf(bytes)
	if line_end < 0:
		return false
	var line := bytes.slice(0, line_end).get_string_from_utf8()
	var parts := line.split(" ", false)
	if parts.is_empty():
		item.bytes = bytes.slice(line_end + 2)
		_reply(item, "ERROR\r\n".to_utf8_buffer())
		return true

	# storage commandは宣言されたvalue bytesが揃うまで待つ。
	if parts[0] in ["set", "add", "replace"]:
		if parts.size() != 5 or not parts[3].is_valid_int() or not parts[4].is_valid_int():
			item.bytes = bytes.slice(line_end + 2)
			_reply(item, "CLIENT_ERROR bad command\r\n".to_utf8_buffer())
			return true
		var size := parts[4].to_int()
		var data_at := line_end + 2
		if size < 0 or bytes.size() < data_at + size + 2:
			return false
		if bytes[data_at + size] != 13 or bytes[data_at + size + 1] != 10:
			item.bytes = PackedByteArray()
			_reply(item, "CLIENT_ERROR bad data\r\n".to_utf8_buffer())
			return true
		var key := parts[1]
		var old = _entry(key)
		var allowed := parts[0] == "set" or (parts[0] == "add" and old == null) or (parts[0] == "replace" and old != null)
		if allowed:
			var ttl := parts[3].to_int()
			values[key] = {
				"bytes": bytes.slice(data_at, data_at + size),
				"flags": parts[2].to_int(),
				"expires": 0 if ttl == 0 else Time.get_ticks_msec() + ttl * 1000,
			}
		# stall用keyだけ応答を止め、送信後timeoutの結果不明経路を作る。
		if not key.ends_with(":stall"):
			_reply(item, ("STORED\r\n" if allowed else "NOT_STORED\r\n").to_utf8_buffer())
		item.bytes = bytes.slice(data_at + size + 2)
		return true

	item.bytes = bytes.slice(line_end + 2)
	match parts[0]:
		"get":
			var broken = _broken_get(parts[1]) if parts.size() == 2 else null
			if broken != null:
				_reply(item, broken)
				return true
			var out := PackedByteArray()
			for key in parts.slice(1):
				var value = _entry(key)
				if value == null:
					continue
				out.append_array(("VALUE %s %d %d\r\n" % [key, value.flags, value.bytes.size()]).to_utf8_buffer())
				out.append_array(value.bytes)
				out.append_array("\r\n".to_utf8_buffer())
			out.append_array("END\r\n".to_utf8_buffer())
			_reply(item, out)
		"delete":
			if parts.size() == 2 and parts[1].ends_with(":unexpected"):
				_reply(item, "WAT\r\n".to_utf8_buffer())
				return true
			var hit := parts.size() == 2 and _entry(parts[1]) != null
			if hit:
				values.erase(parts[1])
			_reply(item, ("DELETED\r\n" if hit else "NOT_FOUND\r\n").to_utf8_buffer())
		"touch":
			var value = _entry(parts[1]) if parts.size() == 3 else null
			if value != null:
				var ttl := parts[2].to_int()
				value.expires = 0 if ttl == 0 else Time.get_ticks_msec() + ttl * 1000
			_reply(item, ("TOUCHED\r\n" if value != null else "NOT_FOUND\r\n").to_utf8_buffer())
		"incr", "decr":
			_math(item, parts)
		"version":
			_reply(item, "VERSION fake-1.0\r\n".to_utf8_buffer())
		_:
			_reply(item, "ERROR\r\n".to_utf8_buffer())
	return true


# counter値を符号なし整数として更新する。
func _math(item: Dictionary, parts: PackedStringArray):
	var value = _entry(parts[1]) if parts.size() == 3 else null
	if value == null:
		_reply(item, "NOT_FOUND\r\n".to_utf8_buffer())
		return
	var current: int = value.bytes.get_string_from_utf8().to_int()
	var delta := parts[2].to_int()
	current = current + delta if parts[0] == "incr" else max(0, current - delta)
	value.bytes = str(current).to_utf8_buffer()
	_reply(item, (str(current) + "\r\n").to_utf8_buffer())
