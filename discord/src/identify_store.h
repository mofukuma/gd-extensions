/**************************************************************************/
/*  identify_store.h                                                      */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

#pragma once

// Discord IdentifyとRESTの永続上限をSQLiteで判定する。

#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace godot {

// tokenを保存せず、digestごとのIdentify送信数と公式残数を共有する。
class IdentifyStore {
public:
	// Identify送信前の判定結果。
	enum Result {
		GRANTED, // 1件を記録し、送信を許した
		WAIT, // 指定時間後にもう一度確認する
		REFRESH, // Gateway Bot APIから公式残数を取り直す
		FAILED, // SQLiteを安全に更新できなかった
	};

	// Identify送信前の判定と待機時間。
	struct Gate {
		Result result = FAILED; // 送信、待機、更新、失敗の判定
		uint64_t wait_ms = 0; // WAIT時の残り時間
	};

	// Gateway Bot APIの公式残数とrolling上限を共有DBへ反映する。
	static bool sync(const String &p_token, int p_remaining, uint64_t p_reset_ms, int p_limit);
	// 上限内ならIdentify 1件を原子的に記録する。
	static Gate take(const String &p_token);
};

// tokenごとのREST global枠とroute bucketをprocess間で共有する。
class RestStore {
public:
	// REST送信前の判定結果。
	enum Result {
		GRANTED, // 1件を記録し、送信を許した
		WAIT, // 指定時間後にもう一度確認する
		FAILED, // SQLiteを安全に更新できなかった
	};

	// REST送信前の判定と待機時間。
	struct Gate {
		Result result = FAILED; // 送信、待機、失敗の判定
		uint64_t wait_ms = 0; // WAIT時の残り時間
		uint64_t grant_until = 0; // GRANTEDを送信に使えるUnix時刻ms
		uint64_t epoch = 0; // 判定時のprocess内制限世代
		uint64_t permit = 0; // invalid応答枠を追跡する予約ID
		String bucket; // GRANTED時にDBから読んだbucket
	};

	// REST送信前のSQLite判定をworkerへ予約する。
	static uint64_t take_async(const String &p_token, const String &p_route, const String &p_major, int p_invalid_limit);
	// workerの判定が終わっていれば結果を受け取る。
	static bool poll(uint64_t p_job, Gate &r_gate);
	// workerが確保した送信枠がまだ新鮮か返す。
	static bool fresh(const Gate &p_gate);
	// 不要になった判定を破棄する。
	static void cancel(uint64_t p_job);
	// Discord応答のbucketと再開までの時間をworkerから反映する。
	static void sync_async(const String &p_token, const String &p_route, const String &p_major, const String &p_bucket,
			uint64_t p_wait_ms, bool p_global, uint64_t p_permit, bool p_invalid);
	// 送信しなかった予約をworkerから解放する。
	static void release_async(uint64_t p_permit);
	// GDExtension解放前にworkerを安全に終了する。
	static void shutdown();
};

} // namespace godot
