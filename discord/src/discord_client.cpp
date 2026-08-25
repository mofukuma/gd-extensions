/**************************************************************************/
/*  discord_client.cpp                                                    */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Discord GDExtensionのGateway、REST queue、rate limit実装。宣言はdiscord_client.h。

#include "discord_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

namespace {

constexpr int GATEWAY_EVENT_MAX = 64; // 1frameで処理するGateway event上限
constexpr int GATEWAY_PACKET_MAX = 2 * 1024 * 1024; // Gateway packet上限bytes
constexpr int GATEWAY_FRAME_MAX = 4 * 1024 * 1024; // 1frameで読むGateway上限bytes
constexpr int GATEWAY_PAYLOAD_MAX = 4096; // Discordが許すGateway payload bytes
constexpr int REST_BODY_MAX = 8 * 1024 * 1024; // REST送信JSONの上限bytes
constexpr int REST_RETRY_MAX = 4; // 429再試行の上限
constexpr int REST_GLOBAL_SAFE = 45; // 公称50件/秒へ持たせる安全幅
constexpr int ROUTE_RESET_MAX = 4096; // route制限記録の上限

// 通信結果を共通Dictionaryへ揃える。
Dictionary reply_of(bool p_ok, int p_status, const Variant &p_data, const String &p_error, const PackedStringArray &p_headers = PackedStringArray()) {
	Dictionary reply;
	reply["ok"] = p_ok;
	reply["status"] = p_status;
	reply["data"] = p_data;
	reply["error"] = p_error;
	reply["headers"] = p_headers;
	return reply;
}

// Discord JSON errorから利用側へ出す短い説明を選ぶ。
String error_text(const Variant &p_data, const String &p_fallback) {
	if (p_data.get_type() != Variant::DICTIONARY) {
		return p_fallback;
	}
	const Dictionary body = p_data;
	const Variant message = body.get("message", Variant());
	return message.get_type() == Variant::STRING && !String(message).is_empty() ? String(message) : p_fallback;
}

// Stringならcontentだけのmessage bodyへ包み、Dictionaryはそのまま使う。
Variant message_body(const Variant &p_message) {
	if (p_message.get_type() == Variant::DICTIONARY) {
		return p_message;
	}
	Dictionary body;
	body["content"] = String(p_message);
	return body;
}

} // namespace

// 遅延失敗をSignalへ流す。
void DiscordCallInternal::fail_later(const Dictionary &p_reply) {
	call_deferred("_finish", p_reply);
}

// 自己参照とclientを保持してREST要求を設定する。
void DiscordCallInternal::begin(const Ref<DiscordCallInternal> &p_self, const Ref<DiscordClient> &p_client,
		HTTPClient::Method p_method, const String &p_path, const String &p_route, const String &p_body) {
	self_hold = p_self;
	client = p_client;
	method = p_method;
	path = p_path;
	route = p_route;
	body = p_body;
}

// 結果を通知して保持参照を片付ける。
void DiscordCallInternal::finish(const Dictionary &p_reply) {
	emit_signal("finished", p_reply);
	client.unref();
	self_hold.unref();
}

// 内部CallのmethodとSignalを登録する。
void DiscordCallInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_finish", "reply"), &DiscordCallInternal::finish);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::DICTIONARY, "reply")));
}

// ticks_msecを短く得る。
uint64_t DiscordWireInternal::now_msec() {
	return Time::get_singleton()->get_ticks_msec();
}

// HTTP response headerを小文字Dictionaryへ直す。
Dictionary DiscordWireInternal::header_map(const PackedStringArray &p_headers) {
	Dictionary out;
	for (const String &line : p_headers) {
		const int at = line.find(":");
		if (at > 0) {
			out[line.substr(0, at).strip_edges().to_lower()] = line.substr(at + 1).strip_edges();
		}
	}
	return out;
}

// JSON文字列をVariantへ直し、壊れていれば元文字列を返す。
Variant DiscordWireInternal::json_value(const String &p_text) {
	if (p_text.is_empty()) {
		return Variant();
	}
	Ref<JSON> json;
	json.instantiate();
	return json->parse(p_text) == OK ? json->get_data() : Variant(p_text);
}

// Gateway URLへversionとencodingを補う。
String DiscordWireInternal::gateway_endpoint(const String &p_url) {
	String out = p_url.strip_edges();
	const int scheme = out.find("://");
	const int host_at = scheme < 0 ? 0 : scheme + 3;
	const int query = out.find("?", host_at);
	const int path = out.find("/", host_at);
	if (path < 0 || (query >= 0 && path > query)) {
		out = query < 0 ? out + "/" : out.insert(query, "/");
	}
	const String join = out.contains("?") ? "&" : "?";
	if (!out.contains("v=")) {
		out += join + String("v=10");
	}
	if (!out.contains("encoding=")) {
		out += String(out.contains("?") ? "&" : "?") + "encoding=json";
	}
	return out;
}

// 平文通信を許せるlocalhost URLか判断する。
bool DiscordWireInternal::local_url(const String &p_url, const String &p_scheme) {
	if (!p_url.begins_with(p_scheme)) {
		return false;
	}
	String authority = p_url.substr(p_scheme.length());
	const int end = authority.find("/");
	if (end >= 0) {
		authority = authority.left(end);
	}
	const int query = authority.find("?");
	if (query >= 0) {
		authority = authority.left(query);
	}
	if (authority.is_empty() || authority.contains("@")) {
		return false;
	}
	String host;
	String port;
	if (authority.begins_with("[")) {
		const int close = authority.find("]");
		if (close < 0) {
			return false;
		}
		host = authority.left(close + 1);
		const String tail = authority.substr(close + 1);
		if (!tail.is_empty()) {
			if (!tail.begins_with(":") || !tail.substr(1).is_valid_int()) {
				return false;
			}
			port = tail.substr(1);
		}
	} else {
		const int colon = authority.rfind(":");
		host = colon < 0 ? authority : authority.left(colon);
		port = colon < 0 ? String() : authority.substr(colon + 1);
		if (!port.is_empty() && !port.is_valid_int()) {
			return false;
		}
	}
	return host == "localhost" || host == "127.0.0.1" || host == "[::1]";
}

// 自動再接続しないclose codeか判断する。
bool DiscordWireInternal::fatal_close(int p_code) {
	return p_code == 4004 || p_code == 4010 || p_code == 4011 || p_code == 4012 || p_code == 4013 || p_code == 4014;
}

// route停止時刻を上限付きで記録する。
void DiscordWireInternal::set_route_reset(const String &p_route, uint64_t p_until) {
	if (!route_reset.has(p_route) && route_reset.size() >= ROUTE_RESET_MAX) {
		route_reset.clear();
	}
	route_reset.insert(p_route, p_until);
}

// REST要求を開始できる時刻か調べる。
bool DiscordWireInternal::rest_allowed(const Ref<DiscordCallInternal> &p_call, uint64_t p_now) {
	if (p_now < rest_global_reset) {
		return false;
	}
	if (p_now - rest_window_at >= 1000) {
		rest_window_at = p_now;
		rest_window_count = 0;
	}
	if (rest_window_count >= REST_GLOBAL_SAFE) {
		return false;
	}
	const HashMap<String, uint64_t>::ConstIterator reset = route_reset.find(p_call->route);
	if (reset && p_now < reset->value) {
		return false;
	}
	if (reset) {
		route_reset.erase(p_call->route);
	}
	return true;
}

// REST queueの次要求を始める。
void DiscordWireInternal::start_rest(uint64_t p_now) {
	if (http || rest_queue.empty()) {
		return;
	}
	const size_t count = rest_queue.size();
	for (size_t i = 0; i < count; i++) {
		Ref<DiscordCallInternal> call = rest_queue.front();
		rest_queue.pop_front();
		if (!rest_allowed(call, p_now)) {
			rest_queue.push_back(call);
			continue;
		}
		rest_active = call;
		break;
	}
	if (rest_active.is_null()) {
		return;
	}

	http = memnew(HTTPRequest);
	http->set_timeout(30.0);
	http->set_body_size_limit(max_response);
	http->set_max_redirects(0);
	add_child(http);
	http->connect("request_completed", Callable(this, "_http_done"), CONNECT_ONE_SHOT);
	PackedStringArray headers;
	headers.push_back("Authorization: Bot " + token);
	headers.push_back("Accept: application/json");
	headers.push_back("User-Agent: DiscordBot (https://github.com/mofukuma/gd-cli, 0.1)");
	if (!rest_active->body.is_empty()) {
		headers.push_back("Content-Type: application/json");
	}
	rest_window_count++;
	const Error err = http->request(api_url + rest_active->path, headers, rest_active->method, rest_active->body);
	if (err != OK) {
		Ref<DiscordCallInternal> call = rest_active;
		rest_active.unref();
		rest_queue_bytes -= MIN(rest_queue_bytes, size_t(call->body.to_utf8_buffer().size()));
		clear_http();
		call->finish(reply_of(false, 0, Variant(), vformat("request could not start: %d", err)));
	}
}

// HTTPRequest完了をrate limitへ反映してCallへ返す。
void DiscordWireInternal::http_done(int64_t p_result, int64_t p_status, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	if (rest_active.is_null()) {
		clear_http();
		return;
	}
	const uint64_t now = now_msec();
	const Dictionary headers = header_map(p_headers);
	const String text = p_body.is_empty() ? String() : String::utf8(reinterpret_cast<const char *>(p_body.ptr()), p_body.size());
	const Variant data = json_value(text);
	const double reset_after = String(headers.get("x-ratelimit-reset-after", "0")).to_float();
	const int remaining = String(headers.get("x-ratelimit-remaining", "1")).to_int();
	if (remaining <= 0 && reset_after > 0.0) {
		set_route_reset(rest_active->route, now + uint64_t(reset_after * 1000.0) + 50);
	}

	// 429はDiscordが返した秒数だけ待ち、同じCallを先頭へ戻す。
	if (p_result == HTTPRequest::RESULT_SUCCESS && p_status == 429 && rest_active->retries < REST_RETRY_MAX) {
		const Dictionary limited = data.get_type() == Variant::DICTIONARY ? Dictionary(data) : Dictionary();
		const double header_retry = String(headers.get("retry-after", "0")).to_float();
		const double fallback = header_retry > 0.0 ? header_retry : (reset_after > 0.0 ? reset_after : 1.0);
		const double retry = CLAMP(double(limited.get("retry_after", fallback)), 0.05, 3600.0);
		const uint64_t until = now + uint64_t(retry * 1000.0) + 50;
		const bool global = bool(limited.get("global", false)) || String(headers.get("x-ratelimit-global", "false")).to_lower() == "true";
		if (global) {
			rest_global_reset = until;
		} else {
			set_route_reset(rest_active->route, until);
		}
		rest_active->retries++;
		rest_queue.push_front(rest_active);
		rest_active.unref();
		clear_http();
		return;
	}

	const bool network_ok = p_result == HTTPRequest::RESULT_SUCCESS;
	const bool ok = network_ok && p_status >= 200 && p_status < 300;
	const String error = ok ? String() : error_text(data, network_ok ? vformat("HTTP %d", p_status) : vformat("network error %d", p_result));
	Ref<DiscordCallInternal> call = rest_active;
	rest_active.unref();
	rest_queue_bytes -= MIN(rest_queue_bytes, size_t(call->body.to_utf8_buffer().size()));
	clear_http();
	call->finish(reply_of(ok, p_status, data, error, p_headers));
	if (p_status == 401) {
		while (!rest_queue.empty()) {
			Ref<DiscordCallInternal> denied = rest_queue.front();
			rest_queue.pop_front();
			rest_queue_bytes -= MIN(rest_queue_bytes, size_t(denied->body.to_utf8_buffer().size()));
			denied->finish(reply_of(false, 401, Variant(), "authorization failed"));
		}
	}
}

// 現HTTPRequestだけを片付ける。
void DiscordWireInternal::clear_http() {
	if (http) {
		http->queue_free();
		http = nullptr;
	}
}

// GatewayへJSON payloadを直送する。
Error DiscordWireInternal::send_payload(int p_op, const Variant &p_data) {
	if (socket.is_null() || socket->get_ready_state() != WebSocketPeer::STATE_OPEN) {
		return ERR_UNAVAILABLE;
	}
	Dictionary payload;
	payload["op"] = p_op;
	payload["d"] = p_data;
	const String text = JSON::stringify(payload);
	if (text.to_utf8_buffer().size() > GATEWAY_PAYLOAD_MAX) {
		return ERR_OUT_OF_MEMORY;
	}
	return socket->send_text(text);
}

// IdentifyかResumeをHello後に送る。
void DiscordWireInternal::identify() {
	if (resume_next && !session_id.is_empty()) {
		Dictionary data;
		data["token"] = token;
		data["session_id"] = session_id;
		data["seq"] = sequence < 0 ? Variant() : Variant(sequence);
		if (send_payload(6, data) != OK) {
			drop_socket(4000, "resume failed");
			schedule_reconnect(true);
		}
		return;
	}
	Dictionary properties;
	properties["os"] = "godot";
	properties["browser"] = "gd-cli";
	properties["device"] = "gd-cli";
	Dictionary data;
	data["token"] = token;
	data["intents"] = intents;
	data["properties"] = properties;
	if (send_payload(2, data) != OK) {
		drop_socket(4000, "identify failed");
		schedule_reconnect(false);
	}
}

// heartbeatを直ちに送る。
void DiscordWireInternal::heartbeat(uint64_t p_now) {
	if (send_payload(1, sequence < 0 ? Variant() : Variant(sequence)) == OK) {
		heartbeat_ack = false;
		heartbeat_sent = p_now;
		heartbeat_at = p_now + heartbeat_ms;
	}
}

// Gateway payloadを解釈して状態とsignalへ反映する。
void DiscordWireInternal::receive_payload(const Dictionary &p_payload, uint64_t p_now) {
	const int op = p_payload.get("op", -1);
	const Variant data = p_payload.get("d", Variant());
	const Variant seq = p_payload.get("s", Variant());
	if (seq.get_type() == Variant::INT) {
		sequence = seq;
	}
	switch (op) {
		case 0: {
			const String type = p_payload.get("t", "");
			if (type == "READY" && data.get_type() == Variant::DICTIONARY) {
				const Dictionary ready_data = data;
				session_id = ready_data.get("session_id", "");
				resume_url = ready_data.get("resume_gateway_url", "");
				ready = true;
				resume_next = true;
				reconnect_count = 0;
				owner->accept_ready(ready_data);
			} else if (type == "RESUMED") {
				ready = true;
				resume_next = true;
				reconnect_count = 0;
				owner->accept_resumed();
			}
			if (started) {
				owner->accept_event(type, data);
			}
		} break;
		case 1:
			heartbeat(p_now);
			break;
		case 7:
			owner->accept_disconnected(4000, "gateway requested reconnect");
			drop_socket(4000, "reconnect");
			schedule_reconnect(true, 0);
			break;
		case 9: {
			const bool can_resume = bool(data) && !session_id.is_empty();
			if (!can_resume) {
				session_id = String();
				resume_url = String();
				sequence = -1;
			}
			owner->accept_disconnected(4000, "invalid session");
			drop_socket(4000, "invalid session");
			schedule_reconnect(can_resume, 1000 + int(p_now % 4000));
		} break;
		case 10: {
			const Dictionary hello_data = data.get_type() == Variant::DICTIONARY ? Dictionary(data) : Dictionary();
			const int interval = hello_data.get("heartbeat_interval", 0);
			if (interval < 1000 || interval > 300000) {
				owner->accept_failed("gateway hello is invalid");
				drop_socket(4002, "invalid hello");
				schedule_reconnect(true);
				break;
			}
			heartbeat_ms = interval;
			hello = true;
			heartbeat_ack = true;
			heartbeat_at = p_now + 1 + ((p_now * 1103515245ULL + 12345ULL) % uint64_t(heartbeat_ms));
			identify();
		} break;
		case 11:
			heartbeat_ack = true;
			latency_ms = heartbeat_sent > 0 ? int(p_now - heartbeat_sent) : -1;
			break;
		default:
			break;
	}
}

// 到着済みGateway packetを上限内で読む。
void DiscordWireInternal::receive_gateway(uint64_t p_now) {
	int frame_bytes = 0;
	for (int i = 0; i < GATEWAY_EVENT_MAX && started && socket.is_valid() && socket->get_available_packet_count() > 0 && frame_bytes < GATEWAY_FRAME_MAX; i++) {
		const PackedByteArray packet = socket->get_packet();
		if (!socket->was_string_packet() || packet.size() > GATEWAY_PACKET_MAX) {
			owner->accept_failed("gateway sent an unsupported packet");
			drop_socket(4002, "unsupported packet");
			schedule_reconnect(true);
			return;
		}
		frame_bytes += packet.size();
		const String text = String::utf8(reinterpret_cast<const char *>(packet.ptr()), packet.size());
		const Variant parsed = json_value(text);
		if (parsed.get_type() != Variant::DICTIONARY) {
			owner->accept_failed("gateway sent invalid JSON");
			drop_socket(4002, "invalid JSON");
			schedule_reconnect(true);
			return;
		}
		receive_payload(parsed, p_now);
	}
}

// 次の接続をbackoff付きで予約する。
void DiscordWireInternal::schedule_reconnect(bool p_resume, int p_delay_ms) {
	if (!started) {
		return;
	}
	resume_next = p_resume && !session_id.is_empty();
	int delay = p_delay_ms;
	if (delay < 0) {
		delay = MIN(60000, 1000 << MIN(reconnect_count, 5));
		delay += int(now_msec() % 500);
	}
	reconnect_count++;
	reconnect_at = now_msec() + delay;
}

// 接続中のWebSocketを閉じる。
void DiscordWireInternal::drop_socket(int p_code, const String &p_reason) {
	if (socket.is_valid() && socket->get_ready_state() != WebSocketPeer::STATE_CLOSED) {
		socket->close(p_code, p_reason);
		socket->poll();
	}
	socket.unref();
	hello = false;
	ready = false;
	heartbeat_ms = 0;
}

// 予約時刻になったGatewayへ接続する。
void DiscordWireInternal::connect_gateway() {
	const String endpoint = gateway_endpoint(resume_next && !resume_url.is_empty() ? resume_url : gateway_url);
	socket.instantiate();
	socket->set_inbound_buffer_size(GATEWAY_PACKET_MAX * 2);
	socket->set_outbound_buffer_size(256 * 1024);
	socket->set_max_queued_packets(128);
	const Error err = socket->connect_to_url(endpoint);
	if (err != OK) {
		socket.unref();
		owner->accept_failed(vformat("gateway connection could not start: %d", err));
		schedule_reconnect(resume_next);
	}
}

// Gatewayの接続、heartbeat、受信、presenceを進める。
void DiscordWireInternal::process_gateway(uint64_t p_now) {
	if (!started) {
		return;
	}
	if (socket.is_null()) {
		if (p_now >= reconnect_at) {
			connect_gateway();
		}
		return;
	}
	socket->poll();
	const WebSocketPeer::State state = socket->get_ready_state();
	if (state == WebSocketPeer::STATE_OPEN) {
		receive_gateway(p_now);
		if (socket.is_null()) {
			return;
		}
		if (hello && p_now >= heartbeat_at) {
			if (!heartbeat_ack) {
				owner->accept_failed("gateway heartbeat was not acknowledged");
				drop_socket(4000, "heartbeat timeout");
				schedule_reconnect(true);
				return;
			}
			heartbeat(p_now);
		}
		drain_presence(p_now);
		return;
	}
	if (state != WebSocketPeer::STATE_CLOSED) {
		return;
	}
	const int code = socket->get_close_code();
	const String reason = socket->get_close_reason();
	socket.unref();
	ready = false;
	hello = false;
	owner->accept_disconnected(code, reason);
	if (fatal_close(code)) {
		started = false;
		owner->accept_failed(vformat("gateway closed with fatal code %d", code));
		return;
	}
	const bool can_resume = !session_id.is_empty() && code != 4007 && code != 4009;
	if (!can_resume) {
		session_id = String();
		resume_url = String();
		sequence = -1;
	}
	schedule_reconnect(can_resume);
}

// 最新presenceを固有制限内で送る。
void DiscordWireInternal::drain_presence(uint64_t p_now) {
	if (!ready || presence_pending.is_empty()) {
		return;
	}
	if (p_now - presence_window_at >= 20000) {
		presence_window_at = p_now;
		presence_window_count = 0;
	}
	if (presence_window_count >= 5) {
		return;
	}
	const Error err = socket->send_text(presence_pending);
	if (err != OK) {
		owner->accept_failed(vformat("presence send failed: %d", err));
		return;
	}
	presence_pending = String();
	presence_window_count++;
}

// HTTP callbackだけを登録する。
void DiscordWireInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_http_done", "result", "status", "headers", "body"), &DiscordWireInternal::http_done);
}

// token、endpoint、資源上限を設定する。
bool DiscordWireInternal::setup(DiscordClient *p_owner, const String &p_token, const Dictionary &p_opts) {
	owner = p_owner;
	token = p_token;
	intents = p_opts.get("intents", 0);
	api_url = String(p_opts.get("api_url", api_url)).trim_suffix("/");
	gateway_url = p_opts.get("gateway_url", gateway_url);
	max_rest_queue = p_opts.get("max_rest_queue", max_rest_queue);
	const int64_t queue_bytes = p_opts.get("max_queue_bytes", int64_t(max_queue_bytes));
	max_response = p_opts.get("max_response", max_response);
	if (token.is_empty() || token.length() > 4096 || token.contains("\r") || token.contains("\n") || intents < 0 ||
			(!api_url.begins_with("https://") && !local_url(api_url, "http://")) ||
			(!gateway_url.begins_with("wss://") && !local_url(gateway_url, "ws://")) ||
			max_rest_queue < 1 || max_rest_queue > 1024 || queue_bytes < 1024 || queue_bytes > 64 * 1024 * 1024 ||
			max_response < 1024 || max_response > 64 * 1024 * 1024) {
		return false;
	}
	max_queue_bytes = size_t(queue_bytes);
	set_process_mode(Node::PROCESS_MODE_ALWAYS);
	set_process(true);
	return true;
}

// Gateway接続と自動再接続を始める。
Error DiscordWireInternal::start() {
	if (started) {
		return ERR_ALREADY_IN_USE;
	}
	started = true;
	resume_next = !session_id.is_empty();
	reconnect_at = now_msec();
	return OK;
}

// Gatewayを閉じて自動再接続を止める。
void DiscordWireInternal::stop(int p_code, const String &p_reason) {
	started = false;
	drop_socket(p_code, p_reason);
}

// 最新presenceを送信待ちへ置く。
Error DiscordWireInternal::set_presence(const Dictionary &p_data) {
	const Variant since = p_data.get("since", Variant());
	const Variant activities = p_data.get("activities", Variant());
	const Variant afk = p_data.get("afk", Variant());
	const String status = p_data.get("status", "");
	const bool status_ok = status == "online" || status == "dnd" || status == "idle" || status == "invisible" || status == "offline";
	if ((since.get_type() != Variant::NIL && since.get_type() != Variant::INT) || activities.get_type() != Variant::ARRAY ||
			afk.get_type() != Variant::BOOL || !status_ok) {
		return ERR_INVALID_PARAMETER;
	}
	Dictionary payload;
	payload["op"] = 3;
	payload["d"] = p_data;
	const String text = JSON::stringify(payload);
	if (text.to_utf8_buffer().size() > GATEWAY_PAYLOAD_MAX) {
		return ERR_OUT_OF_MEMORY;
	}
	presence_pending = text;
	return OK;
}

// REST要求をqueueへ追加する。
bool DiscordWireInternal::enqueue(const Ref<DiscordCallInternal> &p_call) {
	const size_t body_bytes = p_call->body.to_utf8_buffer().size();
	if (rest_queue.size() + (rest_active.is_valid() ? 1 : 0) >= size_t(max_rest_queue) || body_bytes > max_queue_bytes - rest_queue_bytes) {
		return false;
	}
	rest_queue.push_back(p_call);
	rest_queue_bytes += body_bytes;
	return true;
}

// Gatewayと全REST要求を閉じる。
void DiscordWireInternal::shutdown(int p_code, const String &p_reason) {
	set_process(false);
	stop(p_code, p_reason);
	clear_http();
	if (rest_active.is_valid()) {
		rest_active->finish(reply_of(false, 0, Variant(), "client closed"));
		rest_active.unref();
	}
	while (!rest_queue.empty()) {
		rest_queue.front()->finish(reply_of(false, 0, Variant(), "client closed"));
		rest_queue.pop_front();
	}
	rest_queue_bytes = 0;
	presence_pending = String();
}

// READY済みか返す。
bool DiscordWireInternal::is_ready() const {
	return ready;
}

// 最終heartbeat latencyを返す。
int DiscordWireInternal::get_latency_ms() const {
	return latency_ms;
}

// 待ちREST要求数を返す。
int DiscordWireInternal::pending() const {
	return int(rest_queue.size()) + (rest_active.is_valid() ? 1 : 0);
}

// 現session IDを返す。
String DiscordWireInternal::get_session_id() const {
	return session_id;
}

// 毎frame GatewayとRESTを進める。
void DiscordWireInternal::_process(double p_delta) {
	(void)p_delta;
	const Ref<DiscordClient> keep = owner ? Ref<DiscordClient>(Variant(owner)) : Ref<DiscordClient>();
	if (keep.is_null()) {
		return;
	}
	const uint64_t now = now_msec();
	process_gateway(now);
	start_rest(now);
}

// Node解放時に通信を片付ける。
DiscordWireInternal::~DiscordWireInternal() {
	shutdown();
}

// HTTP method名をGodot enumへ直す。
bool DiscordClient::method_of(const String &p_name, HTTPClient::Method &r_method) {
	const String name = p_name.to_upper();
	if (name == "GET") {
		r_method = HTTPClient::METHOD_GET;
	} else if (name == "POST") {
		r_method = HTTPClient::METHOD_POST;
	} else if (name == "PUT") {
		r_method = HTTPClient::METHOD_PUT;
	} else if (name == "PATCH") {
		r_method = HTTPClient::METHOD_PATCH;
	} else if (name == "DELETE") {
		r_method = HTTPClient::METHOD_DELETE;
	} else {
		return false;
	}
	return true;
}

// Discord snowflakeとして安全な数字か確かめる。
bool DiscordClient::snowflake_ok(const String &p_id) {
	if (p_id.is_empty() || p_id.length() > 32) {
		return false;
	}
	for (int i = 0; i < p_id.length(); i++) {
		if (p_id[i] < '0' || p_id[i] > '9') {
			return false;
		}
	}
	return true;
}

// rate limit用にpath中の非主要snowflakeを正規化する。
String DiscordClient::route_of(HTTPClient::Method p_method, const String &p_path) {
	const String clean = p_path.get_slice("?", 0);
	const PackedStringArray parts = clean.split("/", false);
	PackedStringArray route;
	for (int i = 0; i < parts.size(); i++) {
		const bool major = i > 0 && (parts[i - 1] == "channels" || parts[i - 1] == "guilds" || parts[i - 1] == "webhooks");
		route.push_back(parts[i].is_valid_int() && !major ? ":id" : parts[i]);
	}
	String method;
	switch (p_method) {
		case HTTPClient::METHOD_GET:
			method = "GET";
			break;
		case HTTPClient::METHOD_POST:
			method = "POST";
			break;
		case HTTPClient::METHOD_PUT:
			method = "PUT";
			break;
		case HTTPClient::METHOD_PATCH:
			method = "PATCH";
			break;
		case HTTPClient::METHOD_DELETE:
			method = "DELETE";
			break;
		default:
			method = "OTHER";
			break;
	}
	return method + ":/" + String("/").join(route);
}

// REST callを作り、queueまたは遅延失敗を返す。
Signal DiscordClient::rest(HTTPClient::Method p_method, const String &p_path, const Variant &p_body) {
	Ref<DiscordCallInternal> call;
	call.instantiate();
	const Ref<DiscordClient> client = Variant(this);
	const String body = p_body.get_type() == Variant::NIL ? String() : JSON::stringify(p_body);
	call->begin(call, client, p_method, p_path, route_of(p_method, p_path), body);
	const bool path_ok = p_path.begins_with("/") && !p_path.begins_with("//") && p_path.length() <= 2048 &&
			!p_path.contains("://") && !p_path.contains("\r") && !p_path.contains("\n");
	if (!path_ok || body.to_utf8_buffer().size() > REST_BODY_MAX) {
		call->fail_later(reply_of(false, 0, Variant(), "invalid REST request"));
	} else if (!wire || !wire->enqueue(call)) {
		call->fail_later(reply_of(false, 0, Variant(), wire ? "REST queue is full" : "client closed"));
	}
	return Signal(call.ptr(), "finished");
}

// Gateway Readyをsignalへ流す。
void DiscordClient::accept_ready(const Dictionary &p_data) {
	emit_signal("ready", p_data);
}

// Gateway dispatchをsignalへ流す。
void DiscordClient::accept_event(const String &p_name, const Variant &p_data) {
	emit_signal("event", p_name, p_data);
}

// Gateway Resumeをsignalへ流す。
void DiscordClient::accept_resumed() {
	emit_signal("resumed");
}

// Gateway切断をsignalへ流す。
void DiscordClient::accept_disconnected(int p_code, const String &p_reason) {
	emit_signal("disconnected", p_code, p_reason);
}

// 通信失敗をsignalへ流す。
void DiscordClient::accept_failed(const String &p_message) {
	emit_signal("failed", p_message);
}

// tokenと設定から内部通信Nodeを作る。
bool DiscordClient::setup(const String &p_token, const Dictionary &p_opts) {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	const Variant root_value = tree ? tree->call("get_root") : Variant();
	Object *root_object = root_value.get_type() == Variant::OBJECT ? root_value : nullptr;
	Node *root = Object::cast_to<Node>(root_object);
	if (!root) {
		return false;
	}
	wire = memnew(DiscordWireInternal);
	if (!wire->setup(this, p_token, p_opts)) {
		memdelete(wire);
		wire = nullptr;
		return false;
	}
	root->add_child(wire);
	return true;
}

// Gateway接続を始める。
Error DiscordClient::start() {
	return wire ? wire->start() : ERR_UNAVAILABLE;
}

// Gateway接続とREST待ちを閉じる。
void DiscordClient::close(int p_code, const String &p_reason) {
	DiscordWireInternal *closing = wire;
	wire = nullptr;
	if (closing) {
		closing->shutdown(p_code, p_reason);
		closing->queue_free();
	}
}

// Botのpresenceを更新する。
Error DiscordClient::set_presence(const Dictionary &p_data) {
	return wire ? wire->set_presence(p_data) : ERR_UNAVAILABLE;
}

// 任意のDiscord REST endpointを呼ぶ。
Signal DiscordClient::request(const String &p_method, const String &p_path, const Variant &p_body) {
	HTTPClient::Method method;
	if (!method_of(p_method, method)) {
		Ref<DiscordCallInternal> call;
		call.instantiate();
		const Ref<DiscordClient> client = Variant(this);
		call->begin(call, client, HTTPClient::METHOD_GET, String(), String(), String());
		call->fail_later(reply_of(false, 0, Variant(), "unsupported HTTP method"));
		return Signal(call.ptr(), "finished");
	}
	return rest(method, p_path, p_body);
}

// channelへmessageを送る。
Signal DiscordClient::send_message(const String &p_channel_id, const Variant &p_message) {
	if (!snowflake_ok(p_channel_id)) {
		return rest(HTTPClient::METHOD_POST, String(), Variant());
	}
	return rest(HTTPClient::METHOD_POST, "/channels/" + p_channel_id + "/messages", message_body(p_message));
}

// 既存messageを更新する。
Signal DiscordClient::edit_message(const String &p_channel_id, const String &p_message_id, const Variant &p_message) {
	if (!snowflake_ok(p_channel_id) || !snowflake_ok(p_message_id)) {
		return rest(HTTPClient::METHOD_PATCH, String(), Variant());
	}
	return rest(HTTPClient::METHOD_PATCH, "/channels/" + p_channel_id + "/messages/" + p_message_id, message_body(p_message));
}

// 既存messageを削除する。
Signal DiscordClient::delete_message(const String &p_channel_id, const String &p_message_id) {
	if (!snowflake_ok(p_channel_id) || !snowflake_ok(p_message_id)) {
		return rest(HTTPClient::METHOD_DELETE, String(), Variant());
	}
	return rest(HTTPClient::METHOD_DELETE, "/channels/" + p_channel_id + "/messages/" + p_message_id, Variant());
}

// READY済みか返す。
bool DiscordClient::is_ready() const {
	return wire && wire->is_ready();
}

// heartbeat latencyを返す。
int DiscordClient::get_latency_ms() const {
	return wire ? wire->get_latency_ms() : -1;
}

// 待ちREST要求数を返す。
int DiscordClient::pending() const {
	return wire ? wire->pending() : 0;
}

// 現Gateway session IDを返す。
String DiscordClient::get_session_id() const {
	return wire ? wire->get_session_id() : String();
}

// client解放時に内部Nodeを片付ける。
DiscordClient::~DiscordClient() {
	close();
}

// Discord clientの公開methodとsignalを登録する。
void DiscordClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start"), &DiscordClient::start);
	ClassDB::bind_method(D_METHOD("close", "code", "reason"), &DiscordClient::close, DEFVAL(1000), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("set_presence", "data"), &DiscordClient::set_presence);
	ClassDB::bind_method(D_METHOD("request", "method", "path", "body"), &DiscordClient::request, DEFVAL(Variant()));
	ClassDB::bind_method(D_METHOD("send_message", "channel_id", "message"), &DiscordClient::send_message);
	ClassDB::bind_method(D_METHOD("edit_message", "channel_id", "message_id", "message"), &DiscordClient::edit_message);
	ClassDB::bind_method(D_METHOD("delete_message", "channel_id", "message_id"), &DiscordClient::delete_message);
	ClassDB::bind_method(D_METHOD("is_ready"), &DiscordClient::is_ready);
	ClassDB::bind_method(D_METHOD("get_latency_ms"), &DiscordClient::get_latency_ms);
	ClassDB::bind_method(D_METHOD("pending"), &DiscordClient::pending);
	ClassDB::bind_method(D_METHOD("get_session_id"), &DiscordClient::get_session_id);
	ADD_SIGNAL(MethodInfo("ready", PropertyInfo(Variant::DICTIONARY, "data")));
	ADD_SIGNAL(MethodInfo("event", PropertyInfo(Variant::STRING, "name"), PropertyInfo(Variant::NIL, "data")));
	ADD_SIGNAL(MethodInfo("resumed"));
	ADD_SIGNAL(MethodInfo("disconnected", PropertyInfo(Variant::INT, "code"), PropertyInfo(Variant::STRING, "reason")));
	ADD_SIGNAL(MethodInfo("failed", PropertyInfo(Variant::STRING, "message")));
}

// 設定済みBot clientを作る。
Ref<DiscordClient> Discord::bot(const String &p_token, const Dictionary &p_opts) {
	Ref<DiscordClient> client;
	client.instantiate();
	return client->setup(p_token, p_opts) ? client : Ref<DiscordClient>();
}

// Discord Singletonのfactoryと定数を登録する。
void Discord::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bot", "token", "options"), &Discord::bot, DEFVAL(Dictionary()));
	BIND_ENUM_CONSTANT(GUILDS);
	BIND_ENUM_CONSTANT(GUILD_MEMBERS);
	BIND_ENUM_CONSTANT(GUILD_MODERATION);
	BIND_ENUM_CONSTANT(GUILD_PRESENCES);
	BIND_ENUM_CONSTANT(GUILD_MESSAGES);
	BIND_ENUM_CONSTANT(GUILD_MESSAGE_REACTIONS);
	BIND_ENUM_CONSTANT(DIRECT_MESSAGES);
	BIND_ENUM_CONSTANT(DIRECT_MESSAGE_REACTIONS);
	BIND_ENUM_CONSTANT(MESSAGE_CONTENT);
}

} // namespace godot
