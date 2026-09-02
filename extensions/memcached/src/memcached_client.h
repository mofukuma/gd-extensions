/**************************************************************************/
/*  memcached_client.h                                                    */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

#pragma once

// Memcachedのpersistent TCP clientと、GDScript向け非同期APIを宣言する。

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/stream_peer_tcp.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/signal.hpp>

#include <deque>

namespace godot {

class GDMemcachedClient;

// 要求1件を完了まで生かし、結果DictionaryをSignalへ流す。
class GDMemcachedCallInternal : public RefCounted {
	GDCLASS(GDMemcachedCallInternal, RefCounted);

	Ref<GDMemcachedCallInternal> self_hold; // 完了まで自分を生かす参照
	Ref<GDMemcachedClient> client; // TCP処理Nodeを生かすclient

	// 遅延失敗をSignalへ通知する。
	void deliver(const Dictionary &p_reply);

protected:
	// 遅延通知methodと完了Signalを登録する。
	static void _bind_methods();

public:
	// callとclientの寿命を通信完了まで固定する。
	void begin(const Ref<GDMemcachedCallInternal> &p_self, const Ref<GDMemcachedClient> &p_client);
	// 結果を通知して保持参照を外す。
	void finish(const Dictionary &p_reply);
	// 入力検査等の失敗を次frameへ送る。
	void fail_later(const Dictionary &p_reply);
};

// TCP接続、要求queue、protocol解析をSceneTree上で進める内部Node。
class GDMemcachedWireInternal : public Node {
	GDCLASS(GDMemcachedWireInternal, Node);

public:
	// 応答形式を選ぶcommand種別。
	enum Kind {
		KIND_GET,
		KIND_GET_MANY,
		KIND_STORE,
		KIND_DELETE,
		KIND_TOUCH,
		KIND_MATH,
		KIND_VERSION,
	};

	// queueへ渡すprotocol要求。
	struct Request {
		Kind kind = KIND_GET; // 応答parserの種類
		PackedByteArray payload; // serverへ送るcommand bytes
		Ref<GDMemcachedCallInternal> call; // 完了先
		Dictionary keys; // wire keyから利用側keyへの対応
		bool side_effect = false; // timeout時に結果不明となる操作か
		uint64_t queued_at = 0; // queue待ちを含む期限の起点
	};

private:
	String host; // 接続先host
	int port = 11211; // 接続先port
	int timeout_ms = 500; // 要求全体の上限時間
	int idle_ms = 30000; // 未使用接続を閉じる時間
	int max_response = 16 * 1024 * 1024; // 応答buffer上限
	int64_t max_queue_bytes = 16 * 1024 * 1024; // 待ちpayload合計上限
	Ref<StreamPeerTCP> peer; // 再利用するTCP接続
	std::deque<Request> queue; // 直列に処理する要求
	int64_t queue_bytes = 0; // queueが保持するpayload合計bytes
	PackedByteArray input; // 現要求の受信bytes
	Dictionary parsed_values; // 断片GET応答で解析済みの値
	Dictionary parsed_flags; // 断片GET応答で解析済みのflags
	int parse_at = 0; // input内の次VALUE開始位置
	int scan_at = 0; // 未完成lineでCRLF探索を再開する位置
	int64_t response_bytes = 0; // 現要求で累積受信したbytes数
	int write_at = 0; // 現要求で送信済みのbytes数
	bool sent = false; // 現要求を1 byte以上送信したか
	uint64_t last_used = 0; // idle判定に使う最終利用時刻
	String resolved_host; // 非同期名前解決済みの接続先IP
	int resolver = -1; // Godot IP resolverの要求ID
	int ip_type = 3; // 名前解決で許すIP family

	// 待機中の名前解決要求を破棄する。
	void cancel_resolver();
	// 接続を破棄し、次要求が新規接続する状態へ戻す。
	void drop_connection();
	// 現要求を失敗させ、壊れた接続を破棄する。
	void fail_current(const String &p_msg, const String &p_kind);
	// 現要求のbufferを解析し、完成時に結果を返す。
	int parse_current(Dictionary &r_reply, String &r_error);
	// 現要求を完了してqueueの次へ進める。
	void finish_current(const Dictionary &p_reply);
	// queue先頭を外し、完了通知用callを返す。
	Ref<GDMemcachedCallInternal> take_current();

protected:
	// 内部Nodeに公開methodを持たせないため空の登録口を置く。
	static void _bind_methods();

public:
	// 接続先と資源上限を設定する。
	void setup(const String &p_host, int p_port, int p_timeout_ms, int p_idle_ms, int p_max_response, int64_t p_max_queue_bytes, int p_ip_type);
	// 要求をqueueへ追加する。
	void enqueue(const Request &p_request);
	// payloadを総byte上限内でqueueへ追加できるか調べる。
	bool can_enqueue(int p_bytes) const;
	// 現在待っている要求数を返す。
	int pending() const;
	// 全要求を失敗させ、接続を閉じる。
	void shutdown();
	// 毎frame TCPとprotocolを進める。
	void _process(double p_delta) override;
	// Node解放時に名前解決要求を片付ける。
	~GDMemcachedWireInternal();
};

// key prefix、value codec、主要cache操作を持つ公開client。
class GDMemcachedClient : public RefCounted {
	GDCLASS(GDMemcachedClient, RefCounted);

	GDMemcachedWireInternal *wire = nullptr; // SceneTree上のTCP処理Node
	String prefix; // 全keyへ足すnamespace
	int max_value = 1024 * 1024; // 送信valueの上限bytes
	int max_pending = 1024; // queueへ保持する要求上限

	// prefix適用後のkeyをprotocol制約へ合わせる。
	bool wire_key(const String &p_key, String &r_key) const;
	// Variantをflags付きbytesへ変換する。
	bool encode(const Variant &p_value, PackedByteArray &r_bytes, uint32_t &r_flags) const;
	// callを作り、通信要求または遅延失敗を返す。
	Signal request(GDMemcachedWireInternal::Request p_request, const String &p_error = String(), const String &p_kind = "invalid_data");
	// storage commandを共通形式で作る。
	Signal store(const String &p_command, const String &p_key, const Variant &p_value, int64_t p_ttl);
	// keyと数値だけのcommandを共通形式で作る。
	Signal key_number(const String &p_command, const String &p_key, int64_t p_value, GDMemcachedWireInternal::Kind p_kind, bool p_side_effect);

	friend class GDMemcached;

protected:
	// Memcached clientの公開methodを登録する。
	static void _bind_methods();

public:
	// SceneTreeへTCP処理Nodeを作り、client設定を反映する。
	bool setup(const String &p_host, int p_port, const Dictionary &p_opts);
	// keyのvalueを取得する。missはhit=falseで返す。
	Signal get(const String &p_key);
	// 複数keyを1往復で取得する。
	Signal get_many(const PackedStringArray &p_keys);
	// Variantをcodec付きで保存する。
	Signal set(const String &p_key, const Variant &p_value, int64_t p_ttl = 0);
	// raw bytesとflagsをそのまま保存する。
	Signal set_raw(const String &p_key, const PackedByteArray &p_value, int64_t p_flags = 0, int64_t p_ttl = 0);
	// keyが無い場合だけ保存する。
	Signal add(const String &p_key, const Variant &p_value, int64_t p_ttl = 0);
	// keyが有る場合だけ保存する。
	Signal replace(const String &p_key, const Variant &p_value, int64_t p_ttl = 0);
	// keyを削除する。
	Signal remove(const String &p_key);
	// keyの期限を更新する。
	Signal touch(const String &p_key, int64_t p_ttl);
	// unsigned counterを増やす。
	Signal increment(const String &p_key, int64_t p_delta = 1);
	// unsigned counterを減らす。
	Signal decrement(const String &p_key, int64_t p_delta = 1);
	// server versionを得る。
	Signal version();
	// 待ち要求とTCP接続を閉じる。
	void close();
	// client解放時に内部Nodeを片付ける。
	~GDMemcachedClient();
};

// Memcached clientの生成口を大域Singletonへまとめる。
class GDMemcached : public Object {
	GDCLASS(GDMemcached, Object);

protected:
	// Memcached Singletonのfactoryを登録する。
	static void _bind_methods();

public:
	// 設定済みclientを作る。
	Ref<GDMemcachedClient> client(const String &p_host = "127.0.0.1", int p_port = 11211, const Dictionary &p_opts = Dictionary());
};

} // namespace godot
