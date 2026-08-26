/**************************************************************************/
/*  discord_client.h                                                      */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

#pragma once

// Discord GatewayとRESTを、本家のWebSocketPeerとHTTPRequestで提供する。
// GDScriptからはDiscord Singletonのbot()で受け取り、型名は書かない。

#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/http_request.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/web_socket_peer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/signal.hpp>

#include <deque>

namespace godot {

class DiscordClient;

// REST要求1件を完了まで生かし、結果DictionaryをSignalへ流す。
class DiscordCallInternal : public RefCounted {
	GDCLASS(DiscordCallInternal, RefCounted);

	Ref<DiscordCallInternal> self_hold; // 完了まで自分を生かす参照
	Ref<DiscordClient> client; // 通信中にclientを生かす参照
	HTTPClient::Method method = HTTPClient::METHOD_GET; // REST method
	String path; // API基準URLからの相対path
	String route; // rate limitを共有する正規化route
	String bucket; // 送信時にSQLiteから読んだbucket
	String body; // JSON request body
	uint64_t permit = 0; // invalid応答枠の予約ID
	int retries = 0; // 429を再送した回数

	friend class DiscordWireInternal;
	friend class DiscordClient;

	// 遅延失敗をSignalへ流す。
	void fail_later(const Dictionary &p_reply);

protected:
	// 内部CallのmethodとSignalを登録する。
	static void _bind_methods();

public:
	// 自己参照とclientを保持してREST要求を設定する。
	void begin(const Ref<DiscordCallInternal> &p_self, const Ref<DiscordClient> &p_client,
			HTTPClient::Method p_method, const String &p_path, const String &p_route, const String &p_body);
	// 結果を通知して保持参照を片付ける。
	void finish(const Dictionary &p_reply);
};

// SceneTree上でGatewayとREST queueを毎frame進める内部Node。
class DiscordWireInternal : public Node {
	GDCLASS(DiscordWireInternal, Node);

	DiscordClient *owner = nullptr; // signalを出す公開client
	Ref<WebSocketPeer> socket; // 本家WebSocket transport
	HTTPRequest *http = nullptr; // 現在のREST要求
	std::deque<Ref<DiscordCallInternal>> rest_queue; // RESTの直列待ち
	Ref<DiscordCallInternal> rest_active; // 実行中のREST要求
	std::deque<uint64_t> presence_at; // 過去20秒のpresence送信時刻
	String presence_pending; // 次に送る最新presence payload
	String token; // Bot token。外へ表示しない
	String token_key; // SQLiteでBot送信枠を共有するtoken digest
	String api_url = "https://discord.com/api/v10"; // REST基準URL
	String gateway_url; // Gateway Bot APIが返す初回URL
	String resume_url; // Readyで受けた再開用URL
	String session_id; // Resumeに必要なsession
	PackedByteArray gateway_pending; // 次frameへ回すGateway packet
	int64_t intents = 0; // Gateway intent bitfield
	int64_t sequence = -1; // 最後に受けたdispatch sequence
	uint64_t heartbeat_at = 0; // 次heartbeat時刻
	uint64_t heartbeat_sent = 0; // latency計測用の送信時刻
	uint64_t reconnect_at = 0; // 次接続試行時刻
	uint64_t identify_at = 0; // 次にSQLite上限を確認する時刻
	uint64_t rest_check_at = 0; // 次にSQLiteのREST上限を確認する時刻
	uint64_t rest_job = 0; // workerで実行中のREST判定ID
	uint64_t close_deadline = 0; // close handshakeを待つ上限時刻
	size_t rest_queue_bytes = 0; // 実行中を含むREST body総bytes
	int heartbeat_ms = 0; // Helloで得た間隔
	int latency_ms = -1; // 最終heartbeatの往復ms
	int reconnect_count = 0; // 指数backoff用の連続失敗数
	int max_rest_queue = 256; // 待たせるREST要求数
	int max_gateway_packet = 2 * 1024 * 1024; // Gateway packetと1frameの上限bytes
	int identify_limit = 900; // rolling 24時間のIdentify上限
	int invalid_limit = 900; // rolling 10分のinvalid応答上限
	size_t max_queue_bytes = 16 * 1024 * 1024; // REST body総量上限bytes
	int max_response = 8 * 1024 * 1024; // REST応答上限bytes
	bool started = false; // 自動再接続を行う状態
	bool ready = false; // READYまたはRESUMED受信済み
	bool hello = false; // Hello受信済み
	bool heartbeat_ack = true; // 最終heartbeatへACK済み
	bool resume_next = true; // 次接続でResumeを試す
	bool identify_pending = false; // Hello後のIdentify送信待ち
	bool gateway_pending_text = false; // 保留packetのtext種別
	bool gateway_loading = false; // Gateway Bot APIの応答待ち
	bool authorized = true; // 401後に新しいRESTを止める状態
	bool closing = false; // close handshakeの完了待ち

	// ticks_msecを短く得る。
	static uint64_t now_msec();
	// HTTP response headerを小文字Dictionaryへ直す。
	static Dictionary header_map(const PackedStringArray &p_headers);
	// JSON文字列をVariantへ直し、壊れていれば元文字列を返す。
	static Variant json_value(const String &p_text);
	// Gateway URLへversionとencodingを補う。
	static String gateway_endpoint(const String &p_url);
	// 平文通信を許せるlocalhost URLか判断する。
	static bool local_url(const String &p_url, const String &p_scheme);
	// tokenを送れるGateway URLか判断する。
	static bool safe_gateway_url(const String &p_url);
	// 自動再接続しないclose codeか判断する。
	static bool fatal_close(int p_code);
	// REST queueの次要求を始める。
	void start_rest(uint64_t p_now);
	// HTTPRequest完了をrate limitへ反映してCallへ返す。
	void http_done(int64_t p_result, int64_t p_status, const PackedStringArray &p_headers, const PackedByteArray &p_body);
	// 現HTTPRequestだけを片付ける。
	void clear_http();
	// GatewayへJSON payloadを直送する。
	Error send_payload(int p_op, const Variant &p_data);
	// IdentifyかResumeをHello後に送る。
	void identify(uint64_t p_now);
	// heartbeatを直ちに送る。
	void heartbeat(uint64_t p_now);
	// Gateway payloadを解釈して状態とsignalへ反映する。
	void receive_payload(const Dictionary &p_payload, uint64_t p_now);
	// 到着済みGateway packetを上限内で読む。
	void receive_gateway(uint64_t p_now);
	// 次の接続をbackoff付きで予約する。
	void schedule_reconnect(bool p_resume, int p_delay_ms = -1);
	// 接続中のWebSocketを閉じる。
	void drop_socket(int p_code = 1000, const String &p_reason = String());
	// 予約時刻になったGatewayへ接続する。
	void connect_gateway();
	// Gatewayの接続、heartbeat、受信、presenceを進める。
	void process_gateway(uint64_t p_now);
	// 最新presenceを固有制限内で送る。
	void drain_presence(uint64_t p_now);
	// Gateway Bot APIから接続先と公式残数を取得する。
	void load_gateway();

protected:
	// HTTP callbackとGateway開始callbackを登録する。
	static void _bind_methods();
	// Gateway Bot APIのURLとIdentify枠を受け取る。
	void gateway_info(const Dictionary &p_reply);

public:
	// token、endpoint、資源上限を設定する。
	bool setup(DiscordClient *p_owner, const String &p_token, const Dictionary &p_opts);
	// Gateway接続と自動再接続を始める。
	Error start();
	// 最新presenceを送信待ちへ置く。
	Error set_presence(const Dictionary &p_data);
	// REST要求をqueueへ追加する。
	bool enqueue(const Ref<DiscordCallInternal> &p_call);
	// Gatewayと全REST要求を閉じる。
	void shutdown(int p_code = 1000, const String &p_reason = String());
	// READY済みか返す。
	bool is_ready() const;
	// 最終heartbeat latencyを返す。
	int get_latency_ms() const;
	// 待ちREST要求数を返す。
	int pending() const;
	// 現session IDを返す。
	String get_session_id() const;
	// 毎frame GatewayとRESTを進める。
	void _process(double p_delta) override;
	// Node解放時に通信を片付ける。
	~DiscordWireInternal();
};

// Gateway signalとRESTの短いAPIを持つ公開client。
class DiscordClient : public RefCounted {
	GDCLASS(DiscordClient, RefCounted);

	DiscordWireInternal *wire = nullptr; // SceneTree上の通信処理Node

	// HTTP method名をGodot enumへ直す。
	static bool method_of(const String &p_name, HTTPClient::Method &r_method);
	// Discord snowflakeとして安全な数字か確かめる。
	static bool snowflake_ok(const String &p_id);
	// rate limit用にpath中の非主要snowflakeを正規化する。
	static String route_of(HTTPClient::Method p_method, const String &p_path);
	// REST callを作り、queueまたは遅延失敗を返す。
	Signal rest(HTTPClient::Method p_method, const String &p_path, const Variant &p_body);
	// Gateway Readyをsignalへ流す。
	void accept_ready(const Dictionary &p_data);
	// Gateway dispatchをsignalへ流す。
	void accept_event(const String &p_name, const Variant &p_data);
	// Gateway Resumeをsignalへ流す。
	void accept_resumed();
	// Gateway切断をsignalへ流す。
	void accept_disconnected(int p_code, const String &p_reason);
	// 通信失敗をsignalへ流す。
	void accept_failed(const String &p_message);

	friend class DiscordWireInternal;
	friend class Discord;

protected:
	// Discord clientの公開methodとsignalを登録する。
	static void _bind_methods();

public:
	// tokenと設定から内部通信Nodeを作る。
	bool setup(const String &p_token, const Dictionary &p_opts);
	// Gateway接続を始める。
	Error start();
	// Gateway接続とREST待ちを閉じる。
	void close(int p_code = 1000, const String &p_reason = String());
	// Botのpresenceを更新する。
	Error set_presence(const Dictionary &p_data);
	// 任意のDiscord REST endpointを呼ぶ。
	Signal request(const String &p_method, const String &p_path, const Variant &p_body = Variant());
	// channelへmessageを送る。
	Signal send_message(const String &p_channel_id, const Variant &p_message);
	// 既存messageを更新する。
	Signal edit_message(const String &p_channel_id, const String &p_message_id, const Variant &p_message);
	// 既存messageを削除する。
	Signal delete_message(const String &p_channel_id, const String &p_message_id);
	// READY済みか返す。
	bool is_ready() const;
	// heartbeat latencyを返す。
	int get_latency_ms() const;
	// 待ちREST要求数を返す。
	int pending() const;
	// 現Gateway session IDを返す。
	String get_session_id() const;
	// client解放時に内部Nodeを片付ける。
	~DiscordClient();
};

// Discord clientの生成口とintent定数を大域Singletonへまとめる。
class Discord : public Object {
	GDCLASS(Discord, Object);

public:
	// Discord Gateway intentの主要bit。
	enum Intent {
		GUILDS = 1 << 0,
		GUILD_MEMBERS = 1 << 1,
		GUILD_MODERATION = 1 << 2,
		GUILD_PRESENCES = 1 << 8,
		GUILD_MESSAGES = 1 << 9,
		GUILD_MESSAGE_REACTIONS = 1 << 10,
		DIRECT_MESSAGES = 1 << 12,
		DIRECT_MESSAGE_REACTIONS = 1 << 13,
		MESSAGE_CONTENT = 1 << 15,
	};

protected:
	// Discord Singletonのfactoryと定数を登録する。
	static void _bind_methods();

public:
	// 設定済みBot clientを作る。
	Ref<DiscordClient> bot(const String &p_token, const Dictionary &p_opts = Dictionary());
};

} // namespace godot

VARIANT_ENUM_CAST(godot::Discord::Intent);
