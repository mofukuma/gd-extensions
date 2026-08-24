/**************************************************************************/
/*  memcached_client.cpp                                                  */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Memcached GDExtensionのcodec、TCP queue、basic text protocol実装。

#include "memcached_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/ip.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <chrono>

namespace godot {

static constexpr uint32_t FLAG_MAGIC = 0x47440000; // gd codecを示すflags上位bits
static constexpr uint32_t FLAG_STRING = FLAG_MAGIC | 1; // UTF-8 String
static constexpr uint32_t FLAG_JSON = FLAG_MAGIC | 2; // JSON対応Variant
static constexpr uint32_t FLAG_INT = FLAG_MAGIC | 3; // 10進整数
static constexpr int PARSE_WAIT = 0; // 応答bytes待ち
static constexpr int PARSE_DONE = 1; // 応答完成
static constexpr int PARSE_ERROR = -1; // protocol不整合
static constexpr int IO_CHUNK = 1024 * 1024; // TCPを1回処理する最大bytes
static constexpr int IO_BUDGET = 4 * 1024 * 1024; // 1frameの送受信上限bytes
static constexpr int RESPONSE_OVERHEAD = 512; // VALUE headerと終端用の余白bytes

// engine Singletonを保持せず単調増加時刻を得る。
static uint64_t ticks_msec() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 成功replyをR.ofでも読める共通形へ揃える。
static Dictionary ok_reply(const Variant &p_value = Variant()) {
	Dictionary out;
	out["ok"] = true;
	out["value"] = p_value;
	out["msg"] = String();
	out["kind"] = String();
	return out;
}

// 失敗replyをR.ofでも読める共通形へ揃える。
static Dictionary error_reply(const String &p_msg, const String &p_kind) {
	Dictionary out;
	out["ok"] = false;
	out["value"] = Variant();
	out["msg"] = p_msg;
	out["kind"] = p_kind;
	return out;
}

// bytes内のCRLF開始位置を探す。
static int find_crlf(const PackedByteArray &p_bytes, int p_from = 0) {
	const uint8_t *data = p_bytes.ptr();
	for (int i = MAX(0, p_from); i + 1 < p_bytes.size(); i++) {
		if (data[i] == '\r' && data[i + 1] == '\n') {
			return i;
		}
	}
	return -1;
}

// bytes範囲をUTF-8 Stringへ変換する。
static String text_range(const PackedByteArray &p_bytes, int p_at, int p_size) {
	return String::utf8(reinterpret_cast<const char *>(p_bytes.ptr() + p_at), p_size);
}

// 応答が符号なし10進整数だけで構成されるか調べる。
static bool is_unsigned_decimal(const String &p_text) {
	if (p_text.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_text.length(); i++) {
		if (p_text[i] < '0' || p_text[i] > '9') {
			return false;
		}
	}
	return true;
}

// gd codecのflagsだけをVariantへ戻し、未知flagsはraw bytesで保つ。
static Variant decode_value(const PackedByteArray &p_bytes, uint32_t p_flags) {
	if ((p_flags & 0xffff0000) != FLAG_MAGIC) {
		return p_bytes;
	}
	const String text = String::utf8(reinterpret_cast<const char *>(p_bytes.ptr()), p_bytes.size());
	if (p_flags == FLAG_STRING) {
		return text;
	}
	if (p_flags == FLAG_INT && text.is_valid_int()) {
		return text.to_int();
	}
	if (p_flags == FLAG_JSON) {
		Ref<JSON> json;
		json.instantiate();
		if (json->parse(text) == OK) {
			return json->get_data();
		}
	}
	return p_bytes;
}

// serverのerror lineをreply用の種別と文へ分ける。
static Dictionary line_error(const String &p_line) {
	if (p_line.begins_with("CLIENT_ERROR")) {
		return error_reply(p_line.trim_prefix("CLIENT_ERROR").strip_edges(), "invalid_data");
	}
	if (p_line.begins_with("SERVER_ERROR")) {
		return error_reply(p_line.trim_prefix("SERVER_ERROR").strip_edges(), "unavailable");
	}
	return error_reply(p_line, "protocol");
}

// 遅延失敗をSignalへ通知する。
void MemcachedCallInternal::deliver(const Dictionary &p_reply) {
	finish(p_reply);
}

// callとclientの寿命を通信完了まで固定する。
void MemcachedCallInternal::begin(const Ref<MemcachedCallInternal> &p_self, const Ref<MemcachedClient> &p_client) {
	self_hold = p_self;
	client = p_client;
}

// 結果を通知して保持参照を外す。
void MemcachedCallInternal::finish(const Dictionary &p_reply) {
	emit_signal("finished", p_reply);
	client.unref();
	self_hold.unref();
}

// 入力検査等の失敗を次frameへ送る。
void MemcachedCallInternal::fail_later(const Dictionary &p_reply) {
	call_deferred("_deliver", p_reply);
}

// 遅延通知methodと完了Signalを登録する。
void MemcachedCallInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_deliver", "reply"), &MemcachedCallInternal::deliver);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::DICTIONARY, "reply")));
}

// 待機中の名前解決要求を破棄する。
void MemcachedWireInternal::cancel_resolver() {
	if (resolver != IP::RESOLVER_INVALID_ID) {
		IP::get_singleton()->erase_resolve_item(resolver);
		resolver = IP::RESOLVER_INVALID_ID;
	}
}

// 接続を破棄し、次要求が新規接続する状態へ戻す。
void MemcachedWireInternal::drop_connection() {
	cancel_resolver();
	if (peer.is_valid()) {
		peer->disconnect_from_host();
		peer.unref();
	}
	input.clear();
	parsed_values = Dictionary();
	parsed_flags = Dictionary();
	parse_at = 0;
	scan_at = 0;
	response_bytes = 0;
	write_at = 0;
	sent = false;
}

// 現要求を失敗させ、壊れた接続を破棄する。
void MemcachedWireInternal::fail_current(const String &p_msg, const String &p_kind) {
	if (queue.empty()) {
		return;
	}
	const Request &request = queue.front();
	const String kind = request.side_effect && sent && (p_kind == "network" || p_kind == "timed_out") ? "ambiguous" : p_kind;
	Ref<MemcachedCallInternal> call = take_current();
	drop_connection();
	if (p_kind == "network" && !host.is_valid_ip_address()) {
		resolved_host = String();
	}
	call->finish(error_reply(p_msg, kind));
}

// 現要求のbufferを解析し、完成時に結果を返す。
int MemcachedWireInternal::parse_current(Dictionary &r_reply, String &r_error) {
	if (queue.empty()) {
		return PARSE_WAIT;
	}
	const Request &request = queue.front();
	if (request.kind == KIND_GET || request.kind == KIND_GET_MANY) {
		int at = parse_at;
		while (true) {
			const int line_end = find_crlf(input, MAX(at, scan_at));
			if (line_end < 0) {
				scan_at = MAX(at, input.size() - 1);
				return PARSE_WAIT;
			}
			scan_at = at;
			const String line = text_range(input, at, line_end - at);
			if (line == "END") {
				r_reply = ok_reply();
				if (request.kind == KIND_GET) {
					r_reply["hit"] = !parsed_values.is_empty();
					r_reply["value"] = parsed_values.is_empty() ? Variant() : parsed_values.values()[0];
					r_reply["flags"] = int64_t(0);
					if (!parsed_values.is_empty()) {
						r_reply["flags"] = parsed_flags.values()[0];
					}
				} else {
					r_reply["value"] = parsed_values;
					r_reply["hits"] = parsed_values.size();
				}
				return PARSE_DONE;
			}
			if (line == "ERROR" || line.begins_with("CLIENT_ERROR") || line.begins_with("SERVER_ERROR")) {
				r_reply = line_error(line);
				return PARSE_DONE;
			}
			const PackedStringArray parts = line.split(" ", false);
			if (parts.size() != 4 || parts[0] != "VALUE" || !parts[2].is_valid_int() || !parts[3].is_valid_int()) {
				r_error = "bad VALUE line";
				return PARSE_ERROR;
			}
			const int64_t size64 = parts[3].to_int();
			if (size64 < 0 || size64 > max_response) {
				r_error = "value length is out of range";
				return PARSE_ERROR;
			}
			const int size = int(size64);
			const int data_at = line_end + 2;
			if (input.size() < data_at + size + 2) {
				return PARSE_WAIT;
			}
			if (input[data_at + size] != '\r' || input[data_at + size + 1] != '\n') {
				r_error = "value terminator is missing";
				return PARSE_ERROR;
			}
			const String wire_key = parts[1];
			if (!request.keys.has(wire_key)) {
				r_error = "server returned an unrequested key";
				return PARSE_ERROR;
			}
			const String user_key = request.keys.get(wire_key, wire_key);
			const int64_t flags64 = parts[2].to_int();
			if (flags64 < 0 || flags64 > UINT32_MAX) {
				r_error = "value flags are out of range";
				return PARSE_ERROR;
			}
			const uint32_t flags = uint32_t(flags64);
			const PackedByteArray bytes = input.slice(data_at, data_at + size);
			parsed_values[user_key] = decode_value(bytes, flags);
			parsed_flags[user_key] = int64_t(flags);
			at = data_at + size + 2;
			parse_at = at;
			scan_at = at;
		}
	}

	const int line_end = find_crlf(input, scan_at);
	if (line_end < 0) {
		scan_at = MAX(0, input.size() - 1);
		return PARSE_WAIT;
	}
	scan_at = 0;
	const String line = text_range(input, 0, line_end);
	if (line == "ERROR" || line.begins_with("CLIENT_ERROR") || line.begins_with("SERVER_ERROR")) {
		r_reply = line_error(line);
		return PARSE_DONE;
	}

	switch (request.kind) {
		case KIND_STORE: {
			if (line == "STORED") {
				r_reply = ok_reply(true);
				return PARSE_DONE;
			}
			if (line == "NOT_STORED" || line == "EXISTS" || line == "NOT_FOUND") {
				r_reply = error_reply(line.to_lower(), "not_stored");
				return PARSE_DONE;
			}
		} break;
		case KIND_DELETE: {
			if (line == "DELETED" || line == "NOT_FOUND") {
				r_reply = ok_reply(line == "DELETED");
				r_reply["hit"] = line == "DELETED";
				return PARSE_DONE;
			}
		} break;
		case KIND_TOUCH: {
			if (line == "TOUCHED" || line == "NOT_FOUND") {
				r_reply = ok_reply(line == "TOUCHED");
				r_reply["hit"] = line == "TOUCHED";
				return PARSE_DONE;
			}
		} break;
		case KIND_MATH: {
			if (line == "NOT_FOUND") {
				r_reply = ok_reply();
				r_reply["hit"] = false;
				return PARSE_DONE;
			}
			if (is_unsigned_decimal(line)) {
				// GDScriptのsigned intを越えた値は桁落ちさせず文字列で返す。
				r_reply = ok_reply(line.is_valid_int() ? Variant(line.to_int()) : Variant(line));
				r_reply["hit"] = true;
				return PARSE_DONE;
			}
		} break;
		case KIND_VERSION: {
			if (line.begins_with("VERSION ")) {
				r_reply = ok_reply(line.trim_prefix("VERSION "));
				return PARSE_DONE;
			}
		} break;
		default:
			break;
	}

	r_error = "unexpected response: " + line;
	return PARSE_ERROR;
}

// 現要求を完了してqueueの次へ進める。
void MemcachedWireInternal::finish_current(const Dictionary &p_reply) {
	if (queue.empty()) {
		return;
	}
	Ref<MemcachedCallInternal> call = take_current();
	input.clear();
	parsed_values = Dictionary();
	parsed_flags = Dictionary();
	parse_at = 0;
	scan_at = 0;
	response_bytes = 0;
	write_at = 0;
	sent = false;
	last_used = ticks_msec();
	call->finish(p_reply);
}

// queue先頭を外し、完了通知用callを返す。
Ref<MemcachedCallInternal> MemcachedWireInternal::take_current() {
	Ref<MemcachedCallInternal> call = queue.front().call;
	queue_bytes -= queue.front().payload.size();
	queue.pop_front();
	return call;
}

// 接続先と資源上限を設定する。
void MemcachedWireInternal::setup(const String &p_host, int p_port, int p_timeout_ms, int p_idle_ms, int p_max_response, int64_t p_max_queue_bytes, int p_ip_type) {
	host = p_host;
	port = p_port;
	timeout_ms = p_timeout_ms;
	idle_ms = p_idle_ms;
	max_response = p_max_response;
	max_queue_bytes = p_max_queue_bytes;
	ip_type = p_ip_type;
	set_process(true);
}

// 要求をqueueへ追加する。
void MemcachedWireInternal::enqueue(const Request &p_request) {
	queue.push_back(p_request);
	queue_bytes += p_request.payload.size();
}

// payloadを総byte上限内でqueueへ追加できるか調べる。
bool MemcachedWireInternal::can_enqueue(int p_bytes) const {
	return p_bytes >= 0 && queue_bytes <= max_queue_bytes - p_bytes;
}

// 現在待っている要求数を返す。
int MemcachedWireInternal::pending() const {
	return int(queue.size());
}

// 全要求を失敗させ、接続を閉じる。
void MemcachedWireInternal::shutdown() {
	while (!queue.empty()) {
		Ref<MemcachedCallInternal> call = take_current();
		call->finish(error_reply("client closed", "interrupted"));
	}
	drop_connection();
}

// 毎frame TCPとprotocolを進める。
void MemcachedWireInternal::_process(double p_delta) {
	(void)p_delta;
	const uint64_t now = ticks_msec();
	if (queue.empty()) {
		if (peer.is_valid() && idle_ms > 0 && now - last_used >= uint64_t(idle_ms)) {
			drop_connection();
		}
		return;
	}
	if (now - queue.front().queued_at >= uint64_t(timeout_ms)) {
		fail_current("memcached request timed out", "timed_out");
		return;
	}

	// 必要なときだけ接続を作り、接続完了までpollする。
	if (peer.is_null()) {
		// hostnameはIP resolver queueで解決し、main loopの同期DNSを避ける。
		if (resolved_host.is_empty() && host.is_valid_ip_address()) {
			resolved_host = host;
		}
		if (resolved_host.is_empty()) {
			IP *ip = IP::get_singleton();
			if (resolver == IP::RESOLVER_INVALID_ID) {
				resolver = ip->resolve_hostname_queue_item(host, IP::Type(ip_type));
				if (resolver == IP::RESOLVER_INVALID_ID) {
					fail_current("memcached DNS queue is full", "network");
				}
				return;
			}
			const IP::ResolverStatus status = ip->get_resolve_item_status(resolver);
			if (status == IP::RESOLVER_STATUS_WAITING) {
				return;
			}
			if (status != IP::RESOLVER_STATUS_DONE) {
				fail_current("memcached DNS lookup failed", "network");
				return;
			}
				const String resolved = ip->get_resolve_item_address(resolver);
				cancel_resolver();
				if (resolved.is_empty()) {
					fail_current("memcached DNS lookup returned no address", "network");
					return;
				}
				resolved_host = resolved;
			}
		peer.instantiate();
		// gdでは元hostとの対応を検める専用口を使い、本家Godotでは通常のIP接続へ戻す。
		const Error err = peer->has_method("connect_to_host_resolved") ?
				(Error)(int64_t)peer->call("connect_to_host_resolved", host, resolved_host, port, ip_type) :
				peer->connect_to_host(resolved_host, port);
		if (err != OK) {
			fail_current(vformat("memcached connect failed: %d", err), "network");
			return;
		}
		peer->set_no_delay(true);
	}
	const Error poll_err = peer->poll();
	if (poll_err != OK || peer->get_status() == StreamPeerSocket::STATUS_ERROR) {
		fail_current("memcached connection failed", "network");
		return;
	}
	if (peer->get_status() != StreamPeerSocket::STATUS_CONNECTED) {
		return;
	}

	// main loopを塞がないよう現要求を固定長ずつ送る。
	const PackedByteArray &payload = queue.front().payload;
	int write_budget = IO_BUDGET;
	while (write_at < payload.size() && write_budget > 0) {
		const int chunk_size = MIN(IO_CHUNK, MIN(write_budget, payload.size() - write_at));
		const PackedByteArray chunk = payload.slice(write_at, write_at + chunk_size);
		const Array wrote = peer->put_partial_data(chunk);
		const Error err = Error(int(wrote[0]));
		const int count = int(wrote[1]);
		if (err != OK || count < 0 || count > chunk_size) {
			fail_current(vformat("memcached write failed: %d", err), "network");
			return;
		}
		if (count > 0) {
			sent = true;
			write_at += count;
			write_budget -= count;
		}
		if (count < chunk_size) {
			return;
		}
	}
	if (write_at < payload.size()) {
		return;
	}

	// 到着済みbytesをまとめて読み、宣言長に従って解析する。
	const int available = peer->get_available_bytes();
	if (available > 0) {
		if (available > max_response - response_bytes) {
			fail_current("memcached response is too large", "limited");
			return;
		}
		const int read_size = MIN(available, IO_BUDGET);
		const Array got = peer->get_data(read_size);
		if (int(got[0]) != OK) {
			fail_current("memcached read failed", "network");
			return;
		}
		input.append_array(got[1]);
		response_bytes += read_size;
	}

	Dictionary reply;
	String parse_error;
	const int parsed = parse_current(reply, parse_error);
	if (parsed == PARSE_DONE) {
		finish_current(reply);
	} else if (parsed == PARSE_ERROR) {
		fail_current(parse_error, "protocol");
	} else if (input.size() >= max_response) {
		fail_current("memcached response is too large", "limited");
	} else if (parse_at > 0) {
		// 完了済みVALUE bytesを捨て、次frameの再解析とbuffer保持を避ける。
		const int consumed = parse_at;
		input = input.slice(parse_at);
		parse_at = 0;
		scan_at = MAX(0, scan_at - consumed);
	}
}

// Node解放時に名前解決要求を片付ける。
MemcachedWireInternal::~MemcachedWireInternal() {
	cancel_resolver();
}

// 内部Nodeに公開methodを持たせないため空の登録口を置く。
void MemcachedWireInternal::_bind_methods() {
}

// prefix適用後のkeyをprotocol制約へ合わせる。
bool MemcachedClient::wire_key(const String &p_key, String &r_key) const {
	r_key = prefix + p_key;
	const PackedByteArray bytes = r_key.to_utf8_buffer();
	if (bytes.is_empty() || bytes.size() > 250) {
		return false;
	}
	for (const uint8_t byte : bytes) {
		if (byte <= 0x20 || byte == 0x7f) {
			return false;
		}
	}
	return true;
}

// Variantをflags付きbytesへ変換する。
bool MemcachedClient::encode(const Variant &p_value, PackedByteArray &r_bytes, uint32_t &r_flags) const {
	switch (p_value.get_type()) {
		case Variant::PACKED_BYTE_ARRAY:
			r_bytes = p_value;
			r_flags = 0;
			break;
		case Variant::STRING:
			r_bytes = String(p_value).to_utf8_buffer();
			r_flags = FLAG_STRING;
			break;
		case Variant::INT:
			r_bytes = String::num_int64(int64_t(p_value)).to_utf8_buffer();
			r_flags = FLAG_INT;
			break;
		default:
			r_bytes = JSON::stringify(p_value).to_utf8_buffer();
			r_flags = FLAG_JSON;
			break;
	}
	return r_bytes.size() <= max_value;
}

// callを作り、通信要求または遅延失敗を返す。
Signal MemcachedClient::request(MemcachedWireInternal::Request p_request, const String &p_error, const String &p_kind) {
	Ref<MemcachedCallInternal> call;
	call.instantiate();
	const Ref<MemcachedClient> owner = Variant(this);
	call->begin(call, owner);
	p_request.call = call;
	p_request.queued_at = ticks_msec();
	if (!p_error.is_empty()) {
		call->fail_later(error_reply(p_error, p_kind));
	} else if (!wire || wire->pending() >= max_pending || !wire->can_enqueue(p_request.payload.size())) {
		call->fail_later(error_reply(wire ? "memcached queue is full" : "memcached client is closed", wire ? "limited" : "interrupted"));
	} else {
		wire->enqueue(p_request);
	}
	return Signal(call.ptr(), "finished");
}

// storage commandを共通形式で作る。
Signal MemcachedClient::store(const String &p_command, const String &p_key, const Variant &p_value, int64_t p_ttl) {
	MemcachedWireInternal::Request request_data;
	request_data.kind = MemcachedWireInternal::KIND_STORE;
	request_data.side_effect = true;
	String key;
	if (!wire_key(p_key, key)) {
		return request(request_data, "memcached key is invalid");
	}
	if (p_ttl < 0 || p_ttl > INT32_MAX) {
		return request(request_data, "memcached ttl is out of range");
	}
	PackedByteArray bytes;
	uint32_t flags = 0;
	if (!encode(p_value, bytes, flags)) {
		return request(request_data, "memcached value is too large", "limited");
	}
	request_data.payload = vformat("%s %s %d %d %d\r\n", p_command, key, int64_t(flags), p_ttl, bytes.size()).to_utf8_buffer();
	request_data.payload.append_array(bytes);
	request_data.payload.append_array(String("\r\n").to_utf8_buffer());
	return request(request_data);
}

// keyと数値だけのcommandを共通形式で作る。
Signal MemcachedClient::key_number(const String &p_command, const String &p_key, int64_t p_value, MemcachedWireInternal::Kind p_kind, bool p_side_effect) {
	MemcachedWireInternal::Request request_data;
	request_data.kind = p_kind;
	request_data.side_effect = p_side_effect;
	String key;
	if (!wire_key(p_key, key)) {
		return request(request_data, "memcached key is invalid");
	}
	if (p_value < 0 || (p_kind == MemcachedWireInternal::KIND_TOUCH && p_value > INT32_MAX)) {
		return request(request_data, "memcached number is out of range");
	}
	request_data.payload = vformat("%s %s %d\r\n", p_command, key, p_value).to_utf8_buffer();
	return request(request_data);
}

// SceneTreeへTCP処理Nodeを作り、client設定を反映する。
bool MemcachedClient::setup(const String &p_host, int p_port, const Dictionary &p_opts) {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	Node *root = tree ? Object::cast_to<Node>(tree->call("get_root")) : nullptr;
	if (!root) {
		return false;
	}
	prefix = p_opts.get("prefix", "");
	max_value = p_opts.get("max_value", 1024 * 1024);
	max_pending = p_opts.get("max_pending", 1024);
	const int timeout_ms = p_opts.get("timeout_ms", 500);
	const int idle_ms = p_opts.get("idle_ms", 30000);
	const int max_response = p_opts.get("max_response", 16 * 1024 * 1024);
	const int64_t max_queue_bytes = p_opts.get("max_queue_bytes", 16 * 1024 * 1024);
	const String ip_mode = p_opts.get("ip_type", "any");
	const int ip_type = ip_mode == "ipv4" ? IP::TYPE_IPV4 : (ip_mode == "ipv6" ? IP::TYPE_IPV6 : (ip_mode == "any" ? IP::TYPE_ANY : -1));
	const int min_timeout = ((MAX(max_value, max_response) + IO_BUDGET - 1) / IO_BUDGET) * 34 + 100;
	if (p_host.is_empty() || p_port < 1 || p_port > 65535 || timeout_ms < 10 || timeout_ms > 60000 ||
			idle_ms < 0 || idle_ms > 3600000 || max_value < 1 || max_value > 128 * 1024 * 1024 ||
			max_pending < 1 || max_pending > 65536 || max_response < max_value + RESPONSE_OVERHEAD || max_response > 128 * 1024 * 1024 ||
			max_queue_bytes < max_value + RESPONSE_OVERHEAD || max_queue_bytes > 512 * 1024 * 1024 || timeout_ms < min_timeout || ip_type < 0) {
		return false;
	}
	wire = memnew(MemcachedWireInternal);
	wire->setup(p_host, p_port, timeout_ms, idle_ms, max_response, max_queue_bytes, ip_type);
	root->add_child(wire);
	return true;
}

// keyのvalueを取得する。missはhit=falseで返す。
Signal MemcachedClient::get(const String &p_key) {
	MemcachedWireInternal::Request request_data;
	request_data.kind = MemcachedWireInternal::KIND_GET;
	String key;
	if (!wire_key(p_key, key)) {
		return request(request_data, "memcached key is invalid");
	}
	request_data.keys[key] = p_key;
	request_data.payload = ("get " + key + "\r\n").to_utf8_buffer();
	return request(request_data);
}

// 複数keyを1往復で取得する。
Signal MemcachedClient::get_many(const PackedStringArray &p_keys) {
	MemcachedWireInternal::Request request_data;
	request_data.kind = MemcachedWireInternal::KIND_GET_MANY;
	if (p_keys.is_empty() || p_keys.size() > 1024) {
		return request(request_data, "memcached key list is empty or too large", "limited");
	}
	PackedStringArray wire_keys;
	for (const String &user_key : p_keys) {
		String key;
		if (!wire_key(user_key, key)) {
			return request(request_data, "memcached key is invalid");
		}
		if (!request_data.keys.has(key)) {
			wire_keys.push_back(key);
			request_data.keys[key] = user_key;
		}
	}
	request_data.payload = ("get " + String(" ").join(wire_keys) + "\r\n").to_utf8_buffer();
	return request(request_data);
}

// Variantをcodec付きで保存する。
Signal MemcachedClient::set(const String &p_key, const Variant &p_value, int64_t p_ttl) {
	return store("set", p_key, p_value, p_ttl);
}

// raw bytesとflagsをそのまま保存する。
Signal MemcachedClient::set_raw(const String &p_key, const PackedByteArray &p_value, int64_t p_flags, int64_t p_ttl) {
	MemcachedWireInternal::Request request_data;
	request_data.kind = MemcachedWireInternal::KIND_STORE;
	request_data.side_effect = true;
	String key;
	if (!wire_key(p_key, key)) {
		return request(request_data, "memcached key is invalid");
	}
	if (p_flags < 0 || p_flags > UINT32_MAX || p_ttl < 0 || p_ttl > INT32_MAX) {
		return request(request_data, "memcached flags or ttl is out of range");
	}
	if (p_value.size() > max_value) {
		return request(request_data, "memcached value is too large", "limited");
	}
	request_data.payload = vformat("set %s %d %d %d\r\n", key, p_flags, p_ttl, p_value.size()).to_utf8_buffer();
	request_data.payload.append_array(p_value);
	request_data.payload.append_array(String("\r\n").to_utf8_buffer());
	return request(request_data);
}

// keyが無い場合だけ保存する。
Signal MemcachedClient::add(const String &p_key, const Variant &p_value, int64_t p_ttl) {
	return store("add", p_key, p_value, p_ttl);
}

// keyが有る場合だけ保存する。
Signal MemcachedClient::replace(const String &p_key, const Variant &p_value, int64_t p_ttl) {
	return store("replace", p_key, p_value, p_ttl);
}

// keyを削除する。
Signal MemcachedClient::remove(const String &p_key) {
	MemcachedWireInternal::Request request_data;
	request_data.kind = MemcachedWireInternal::KIND_DELETE;
	request_data.side_effect = true;
	String key;
	if (!wire_key(p_key, key)) {
		return request(request_data, "memcached key is invalid");
	}
	request_data.payload = ("delete " + key + "\r\n").to_utf8_buffer();
	return request(request_data);
}

// keyの期限を更新する。
Signal MemcachedClient::touch(const String &p_key, int64_t p_ttl) {
	return key_number("touch", p_key, p_ttl, MemcachedWireInternal::KIND_TOUCH, true);
}

// unsigned counterを増やす。
Signal MemcachedClient::increment(const String &p_key, int64_t p_delta) {
	return key_number("incr", p_key, p_delta, MemcachedWireInternal::KIND_MATH, true);
}

// unsigned counterを減らす。
Signal MemcachedClient::decrement(const String &p_key, int64_t p_delta) {
	return key_number("decr", p_key, p_delta, MemcachedWireInternal::KIND_MATH, true);
}

// server versionを得る。
Signal MemcachedClient::version() {
	MemcachedWireInternal::Request request_data;
	request_data.kind = MemcachedWireInternal::KIND_VERSION;
	request_data.payload = String("version\r\n").to_utf8_buffer();
	return request(request_data);
}

// 待ち要求とTCP接続を閉じる。
void MemcachedClient::close() {
	MemcachedWireInternal *closing = wire;
	wire = nullptr;
	if (closing) {
		closing->shutdown();
		closing->queue_free();
	}
}

// client解放時に内部Nodeを片付ける。
MemcachedClient::~MemcachedClient() {
	close();
}

// Memcached clientの公開methodを登録する。
void MemcachedClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get", "key"), &MemcachedClient::get);
	ClassDB::bind_method(D_METHOD("get_many", "keys"), &MemcachedClient::get_many);
	ClassDB::bind_method(D_METHOD("set", "key", "value", "ttl"), &MemcachedClient::set, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_raw", "key", "value", "flags", "ttl"), &MemcachedClient::set_raw, DEFVAL(0), DEFVAL(0));
	ClassDB::bind_method(D_METHOD("add", "key", "value", "ttl"), &MemcachedClient::add, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("replace", "key", "value", "ttl"), &MemcachedClient::replace, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("remove", "key"), &MemcachedClient::remove);
	ClassDB::bind_method(D_METHOD("touch", "key", "ttl"), &MemcachedClient::touch);
	ClassDB::bind_method(D_METHOD("increment", "key", "delta"), &MemcachedClient::increment, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("decrement", "key", "delta"), &MemcachedClient::decrement, DEFVAL(1));
	ClassDB::bind_method(D_METHOD("version"), &MemcachedClient::version);
	ClassDB::bind_method(D_METHOD("close"), &MemcachedClient::close);
}

// 設定済みclientを作る。
Ref<MemcachedClient> Memcached::client(const String &p_host, int p_port, const Dictionary &p_opts) {
	Ref<MemcachedClient> out;
	out.instantiate();
	ERR_FAIL_COND_V_MSG(!out->setup(p_host, p_port, p_opts), Ref<MemcachedClient>(), "Invalid Memcached client settings or missing SceneTree");
	return out;
}

// Memcached Singletonのfactoryを登録する。
void Memcached::_bind_methods() {
	ClassDB::bind_method(D_METHOD("client", "host", "port", "opts"), &Memcached::client, DEFVAL("127.0.0.1"), DEFVAL(11211), DEFVAL(Dictionary()));
}

} // namespace godot
