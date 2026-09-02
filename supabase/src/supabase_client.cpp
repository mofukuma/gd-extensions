/**************************************************************************/
/*  supabase_client.cpp                                                   */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Supabase GDExtensionのHTTP、Database、Auth実装。宣言はsupabase_client.h。

#include "supabase_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/marshalls.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

// JSON応答から外へ出せる失敗文を選ぶ。
static String error_text(const Variant &p_data, const String &p_fallback) {
	if (p_data.get_type() != Variant::DICTIONARY) {
		return p_fallback;
	}
	const Dictionary body = p_data;
	const PackedStringArray keys = { "error_description", "message", "msg", "error" };
	for (const String &key : keys) {
		const String text = body.get(key, "");
		if (!text.is_empty()) {
			return text;
		}
	}
	return p_fallback;
}

// 旧JWT keyのpayloadからservice_role権限を見分ける。
static bool is_legacy_secret(const String &p_key) {
	const PackedStringArray parts = p_key.split(".");
	if (parts.size() != 3) {
		return false;
	}
	String payload = parts[1].replace("-", "+").replace("_", "/");
	while (payload.length() % 4 != 0) {
		payload += "=";
	}
	Ref<JSON> json;
	json.instantiate();
	if (json->parse(Marshalls::get_singleton()->base64_to_utf8(payload)) != OK) {
		return false;
	}
	const Variant data = json->get_data();
	return data.get_type() == Variant::DICTIONARY && Dictionary(data).get("role", "") == "service_role";
}

// HTTP完了値を利用側のDictionaryへ揃える。
void GDSupabaseCallInternal::completed(int64_t p_result, int64_t p_status, const PackedStringArray &p_headers, const PackedByteArray &p_body) {
	const bool network_ok = p_result == HTTPRequest::RESULT_SUCCESS;
	const bool ok = network_ok && p_status >= 200 && p_status < 300;
	String text;
	Variant data;
	if (!p_body.is_empty()) {
		text = String::utf8(reinterpret_cast<const char *>(p_body.ptr()), p_body.size());
		Ref<JSON> json;
		json.instantiate();
		if (json->parse(text) == OK) {
			data = json->get_data();
		} else {
			data = text;
		}
	}

	Dictionary reply;
	reply["ok"] = ok;
	reply["status"] = p_status;
	reply["headers"] = p_headers;
	reply["data"] = data;
	reply["error"] = ok ? String() : error_text(data, network_ok ? vformat("HTTP %d", p_status) : vformat("network error %d", p_result));

	// Auth応答だけclientのsessionへ反映する。
	if (ok && client.is_valid()) {
		if (action == ACTION_SESSION && data.get_type() == Variant::DICTIONARY) {
			client->accept_session(data);
		} else if (action == ACTION_SIGN_OUT) {
			client->clear_session();
		}
	}
	finish(reply);
}

// 開始前の失敗を次frameでSignalへ流す。
void GDSupabaseCallInternal::failed(int64_t p_error) {
	Dictionary reply;
	reply["ok"] = false;
	reply["status"] = 0;
	reply["headers"] = PackedStringArray();
	reply["data"] = Variant();
	reply["error"] = vformat("request could not start: %d", p_error);
	finish(reply);
}

// 結果を通知し、通信Nodeと自己参照を片付ける。
void GDSupabaseCallInternal::finish(const Dictionary &p_reply) {
	emit_signal("finished", p_reply);
	if (request_node) {
		request_node->queue_free();
		request_node = nullptr;
	}
	client.unref();
	self_hold.unref();
}

// HTTPRequestをSceneTreeへ載せて通信を始める。
void GDSupabaseCallInternal::start(const Ref<GDSupabaseCallInternal> &p_self, const Ref<GDSupabaseClient> &p_client,
		const String &p_url, HTTPClient::Method p_method, const PackedStringArray &p_headers,
		const String &p_body, Action p_action) {
	self_hold = p_self;
	client = p_client;
	action = p_action;
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree || !tree->get_root()) {
		call_deferred("_failed", ERR_UNAVAILABLE);
		return;
	}

	request_node = memnew(HTTPRequest);
	request_node->set_timeout(30.0);
	request_node->set_body_size_limit(16 * 1024 * 1024);
	request_node->set_max_redirects(0);
	tree->get_root()->add_child(request_node);
	request_node->connect("request_completed", Callable(this, "_completed"), CONNECT_ONE_SHOT);
	const Error err = request_node->request(p_url, p_headers, p_method, p_body);
	if (err != OK) {
		call_deferred("_failed", err);
	}
}

// 通信CallのmethodとSignalを登録する。
void GDSupabaseCallInternal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_completed", "result", "status", "headers", "body"), &GDSupabaseCallInternal::completed);
	ClassDB::bind_method(D_METHOD("_failed", "error"), &GDSupabaseCallInternal::failed);
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::DICTIONARY, "reply")));
}

// table、function、schemaへ使えるASCII識別子か調べる。
bool GDSupabaseClient::safe_name(const String &p_name) {
	if (p_name.is_empty()) {
		return false;
	}
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
	}
	return true;
}

// query値をpercent encodeしてURLへ繋ぐ。
String GDSupabaseClient::query_text(const Dictionary &p_query) {
	PackedStringArray parts;
	for (const Variant &raw_key : p_query.keys()) {
		const String key = raw_key;
		const String value = p_query[raw_key];
		parts.push_back(key.uri_encode() + "=" + value.uri_encode());
	}
	return parts.is_empty() ? String() : "?" + String("&").join(parts);
}

// 共通認証とschemaの見出しを組む。
PackedStringArray GDSupabaseClient::headers(bool p_json, bool p_mutation, bool p_rest, const PackedStringArray &p_more) const {
	PackedStringArray out;
	out.push_back("apikey: " + api_key);
	out.push_back("Accept: application/json");
	if (p_json) {
		out.push_back("Content-Type: application/json");
	}
	const String token = auth_session.get("access_token", "");
	if (!token.is_empty()) {
		out.push_back("Authorization: Bearer " + token);
	} else if (!api_key.begins_with("sb_publishable_") && !api_key.begins_with("sb_secret_")) {
		out.push_back("Authorization: Bearer " + api_key);
	}
	if (p_rest) {
		out.push_back(String(p_mutation ? "Content-Profile: " : "Accept-Profile: ") + schema);
	}
	for (const Variant &raw_key : extra_headers.keys()) {
		const String key = raw_key;
		const String value = extra_headers[raw_key];
		if (!key.contains("\r") && !key.contains("\n") && !value.contains("\r") && !value.contains("\n")) {
			out.push_back(key + String(": ") + value);
		}
	}
	out.append_array(p_more);
	return out;
}

// REST tableのURLを安全な名前から組む。
String GDSupabaseClient::table_url(const String &p_table, const Dictionary &p_query) const {
	ERR_FAIL_COND_V_MSG(!safe_name(p_table), String(), "table must be an ASCII identifier");
	return base_url + String("/rest/v1/") + p_table + query_text(p_query);
}

// HTTP Callを作り、待てるSignalを返す。
Signal GDSupabaseClient::send(const String &p_url, HTTPClient::Method p_method, const String &p_body,
		const PackedStringArray &p_more, GDSupabaseCallInternal::Action p_action, bool p_rest) {
	Ref<GDSupabaseCallInternal> call;
	call.instantiate();
	const Ref<GDSupabaseClient> owner = Variant(this);
	call->start(call, owner, p_url, p_method, headers(!p_body.is_empty(), p_method != HTTPClient::METHOD_GET, p_rest, p_more), p_body, p_action);
	return Signal(call.ptr(), "finished");
}

// Auth成功時に新しいsessionを控える。
void GDSupabaseClient::accept_session(const Dictionary &p_session) {
	auth_session = p_session;
}

// sign out成功時にsessionを空にする。
void GDSupabaseClient::clear_session() {
	auth_session.clear();
}

// project URL、key、schema等を設定する。
void GDSupabaseClient::setup(const String &p_url, const String &p_key, const Dictionary &p_opts) {
	base_url = p_url.trim_suffix("/");
	api_key = p_key;
	schema = p_opts.get("schema", "public");
	extra_headers = p_opts.get("headers", Dictionary());
}

// tableから行を選ぶ。
Signal GDSupabaseClient::select(const String &p_table, const Dictionary &p_query) {
	Dictionary query = p_query.duplicate();
	if (!query.has("select")) {
		query["select"] = "*";
	}
	return send(table_url(p_table, query), HTTPClient::METHOD_GET, String(), PackedStringArray(), GDSupabaseCallInternal::ACTION_NONE);
}

// tableへ行を追加する。
Signal GDSupabaseClient::insert(const String &p_table, const Variant &p_rows, bool p_upsert) {
	PackedStringArray more;
	more.push_back(String("Prefer: return=representation") + (p_upsert ? ",resolution=merge-duplicates" : ""));
	return send(table_url(p_table, Dictionary()), HTTPClient::METHOD_POST, JSON::stringify(p_rows), more, GDSupabaseCallInternal::ACTION_NONE);
}

// filterに合う行を更新する。
Signal GDSupabaseClient::update(const String &p_table, const Dictionary &p_values, const Dictionary &p_filters) {
	PackedStringArray more;
	more.push_back("Prefer: return=representation");
	return send(table_url(p_table, p_filters), HTTPClient::METHOD_PATCH, JSON::stringify(p_values), more, GDSupabaseCallInternal::ACTION_NONE);
}

// filterに合う行を削除する。
Signal GDSupabaseClient::remove(const String &p_table, const Dictionary &p_filters) {
	PackedStringArray more;
	more.push_back("Prefer: return=representation");
	return send(table_url(p_table, p_filters), HTTPClient::METHOD_DELETE, String(), more, GDSupabaseCallInternal::ACTION_NONE);
}

// PostgreSQL functionを呼ぶ。
Signal GDSupabaseClient::rpc(const String &p_function, const Dictionary &p_args) {
	ERR_FAIL_COND_V_MSG(!safe_name(p_function), Signal(), "function must be an ASCII identifier");
	return send(base_url + "/rest/v1/rpc/" + p_function, HTTPClient::METHOD_POST, JSON::stringify(p_args), PackedStringArray(), GDSupabaseCallInternal::ACTION_NONE);
}

// emailとpasswordでAuth sessionを得る。
Signal GDSupabaseClient::sign_in(const String &p_email, const String &p_password) {
	Dictionary body;
	body["email"] = p_email;
	body["password"] = p_password;
	return send(base_url + "/auth/v1/token?grant_type=password", HTTPClient::METHOD_POST, JSON::stringify(body), PackedStringArray(), GDSupabaseCallInternal::ACTION_SESSION, false);
}

// projectの公開Auth設定を得る。
Signal GDSupabaseClient::auth_settings() {
	return send(base_url + "/auth/v1/settings", HTTPClient::METHOD_GET, String(), PackedStringArray(), GDSupabaseCallInternal::ACTION_NONE, false);
}

// refresh tokenでAuth sessionを更新する。
Signal GDSupabaseClient::refresh(const String &p_refresh_token) {
	const String token = p_refresh_token.is_empty() ? String(auth_session.get("refresh_token", "")) : p_refresh_token;
	Dictionary body;
	body["refresh_token"] = token;
	return send(base_url + "/auth/v1/token?grant_type=refresh_token", HTTPClient::METHOD_POST, JSON::stringify(body), PackedStringArray(), GDSupabaseCallInternal::ACTION_SESSION, false);
}

// server側のsessionを無効化する。
Signal GDSupabaseClient::sign_out() {
	return send(base_url + "/auth/v1/logout", HTTPClient::METHOD_POST, "{}", PackedStringArray(), GDSupabaseCallInternal::ACTION_SIGN_OUT, false);
}

// 外から渡されたsessionを現在値にする。
void GDSupabaseClient::set_session(const Dictionary &p_session) {
	auth_session = p_session;
}

// 現在のsessionを返す。
Dictionary GDSupabaseClient::get_session() const {
	return auth_session.duplicate();
}

// projectの基準URLを返す。
String GDSupabaseClient::get_url() const {
	return base_url;
}

// Supabase clientの公開methodを登録する。
void GDSupabaseClient::_bind_methods() {
	ClassDB::bind_method(D_METHOD("select", "table", "query"), &GDSupabaseClient::select, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("insert", "table", "rows", "upsert"), &GDSupabaseClient::insert, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("update", "table", "values", "filters"), &GDSupabaseClient::update);
	ClassDB::bind_method(D_METHOD("remove", "table", "filters"), &GDSupabaseClient::remove);
	ClassDB::bind_method(D_METHOD("rpc", "function", "args"), &GDSupabaseClient::rpc, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("sign_in", "email", "password"), &GDSupabaseClient::sign_in);
	ClassDB::bind_method(D_METHOD("auth_settings"), &GDSupabaseClient::auth_settings);
	ClassDB::bind_method(D_METHOD("refresh", "refresh_token"), &GDSupabaseClient::refresh, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("sign_out"), &GDSupabaseClient::sign_out);
	ClassDB::bind_method(D_METHOD("set_session", "session"), &GDSupabaseClient::set_session);
	ClassDB::bind_method(D_METHOD("get_session"), &GDSupabaseClient::get_session);
	ClassDB::bind_method(D_METHOD("get_url"), &GDSupabaseClient::get_url);
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "session"), "set_session", "get_session");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "url"), "", "get_url");
}

// 設定済みclientを作る。
Ref<GDSupabaseClient> GDSupabase::client(const String &p_url, const String &p_key, const Dictionary &p_opts) {
	const String url = p_url.trim_suffix("/");
	const bool https = url.begins_with("https://");
	const bool http = url.begins_with("http://");
	const String authority = url.get_slice("://", 1).get_slice("/", 0);
	const bool local = http && (authority == "127.0.0.1" || authority.begins_with("127.0.0.1:") || authority == "localhost" || authority.begins_with("localhost:"));
	const bool clean = !authority.is_empty() && !authority.contains("@") && !url.contains("?") && !url.contains("#") &&
			url == String(https ? "https://" : "http://") + authority;
	ERR_FAIL_COND_V_MSG((!https && !local) || !clean, Ref<GDSupabaseClient>(), "Supabase URL must be an HTTPS origin or localhost");
	ERR_FAIL_COND_V_MSG(p_key.is_empty(), Ref<GDSupabaseClient>(), "Supabase publishable key is required");
	ERR_FAIL_COND_V_MSG(p_key.begins_with("sb_secret_") || is_legacy_secret(p_key), Ref<GDSupabaseClient>(), "Do not use a Supabase secret key in this client");
	const String schema = p_opts.get("schema", "public");
	ERR_FAIL_COND_V_MSG(!GDSupabaseClient::safe_name(schema), Ref<GDSupabaseClient>(), "schema must be an ASCII identifier");
	Ref<GDSupabaseClient> out;
	out.instantiate();
	out->setup(url, p_key, p_opts);
	return out;
}

// Supabase Singletonのfactoryを登録する。
void GDSupabase::_bind_methods() {
	ClassDB::bind_method(D_METHOD("client", "url", "publishable_key", "opts"), &GDSupabase::client, DEFVAL(Dictionary()));
}

} // namespace godot
