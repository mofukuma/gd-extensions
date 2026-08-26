/**************************************************************************/
/*  identify_store.cpp                                                    */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Discord IdentifyとRESTの永続上限をSQLiteで判定する実体。

#include "identify_store.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace godot {

namespace {

constexpr int64_t DAY_MS = 24LL * 60 * 60 * 1000; // rolling集計の24時間ms
constexpr int64_t IDENTIFY_INTERVAL = 5000; // process間でも守るIdentify間隔ms
constexpr int SQLITE_WAIT = 10; // main threadでDB排他を待つ上限ms
constexpr int REST_SQLITE_WAIT = 100; // workerでREST DB排他を待つ上限ms
constexpr int REST_GLOBAL_SAFE = 45; // 公称50件/秒へ持たせる安全幅
constexpr int REST_ROUTE_MAX = 4096; // DB全体に保持するroute対応数
constexpr int64_t REST_WINDOW_MS = 1100; // 非同期枠の遅延も含めるrolling幅ms
constexpr int64_t REST_GRANT_MS = 500; // 低FPSでもpermitを使える期間ms
constexpr int64_t REST_RESERVE_MS = REST_WINDOW_MS + REST_GRANT_MS; // 実送信のrolling幅を守る予約保持ms
constexpr int64_t INVALID_WINDOW_MS = 10 * 60 * 1000; // invalid応答を数える10分ms
constexpr int64_t INVALID_RECHECK_MS = 250; // 未応答予約の解除を再確認する間隔ms
constexpr int64_t DB_MAX_BYTES = 64LL * 1024 * 1024; // 制限DB本体の最大bytes
constexpr size_t REST_SYNC_MAX = 1024; // 終了時にも処理する応答更新上限

// 全projectで共有するOS user固有のDB pathを返す。
String db_path() {
#ifdef _WIN32
	const wchar_t *base = _wgetenv(L"LOCALAPPDATA");
	return base && base[0] ? String(base).path_join("gd_cli_discord_limits.sqlite3") : String();
#else
	const char *base = std::getenv("HOME");
	return base && base[0] ? String::utf8(base).path_join(".gd_cli_discord_limits.sqlite3") : String();
#endif
}

// SQLite接続をscope終了時に必ず閉じる。
class Db {
	sqlite3 *db = nullptr; // 現在のSQLite接続

	// PRAGMAの整数結果を1件読む。
	bool scalar(const char *p_sql, int64_t &r_value) {
		sqlite3_stmt *stmt = nullptr;
		const bool ready = sqlite3_prepare_v2(db, p_sql, -1, &stmt, nullptr) == SQLITE_OK;
		const bool row = ready && sqlite3_step(stmt) == SQLITE_ROW;
		if (row) {
			r_value = sqlite3_column_int64(stmt, 0);
		}
		if (stmt) {
			sqlite3_finalize(stmt);
		}
		return row;
	}

	// 実page sizeからDB本体を64 MiB以内へ制限する。
	bool limit_size() {
		int64_t page_size = 0;
		if (!scalar("PRAGMA page_size;", page_size) || page_size < 512 || page_size > 65536) {
			return false;
		}
		const int64_t pages = DB_MAX_BYTES / page_size;
		const CharString sql = (String("PRAGMA max_page_count=") + String::num_int64(pages) + ";").utf8();
		int64_t actual = 0;
		return exec(sql.get_data()) && scalar("PRAGMA max_page_count;", actual) && actual <= pages;
	}

public:
	// DBを開き、永続上限のtableを準備する。
	bool open(bool p_durable = true, int p_wait = SQLITE_WAIT) {
		const String path = db_path();
		if (path.is_empty()) {
			return false;
		}
		const CharString utf8 = path.utf8();
		if (sqlite3_open_v2(utf8.get_data(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
			return false;
		}
		sqlite3_busy_timeout(db, p_wait);
		if (!exec(p_durable ? "PRAGMA synchronous=FULL; PRAGMA page_size=4096; PRAGMA journal_size_limit=4194304; PRAGMA wal_autocheckpoint=256;" :
						  "PRAGMA synchronous=NORMAL; PRAGMA page_size=4096; PRAGMA journal_size_limit=4194304; PRAGMA wal_autocheckpoint=256;") ||
				!limit_size()) {
			return false;
		}
		static std::atomic_bool ready = false; // process内のtable準備済みか
		if (ready.load(std::memory_order_acquire)) {
			return true;
		}
		static std::mutex schema_mutex; // 初回table作成を1threadに限る
		std::lock_guard<std::mutex> schema_lock(schema_mutex);
		if (ready.load(std::memory_order_relaxed)) {
			return true;
		}
		// 新規DBは4 KiB pageにし、既存DBも実page sizeから容量を制限する。
		const bool prepared = exec("PRAGMA journal_mode=WAL;"
								   "CREATE TABLE IF NOT EXISTS identify_budget("
								   "token TEXT PRIMARY KEY, reset_at INTEGER NOT NULL, remaining INTEGER NOT NULL, "
								   "next_at INTEGER NOT NULL, cap INTEGER NOT NULL) WITHOUT ROWID;"
								   "CREATE TABLE IF NOT EXISTS identify_log("
								   "token TEXT NOT NULL, sent_at INTEGER NOT NULL, PRIMARY KEY(token, sent_at)) WITHOUT ROWID;"
								   "CREATE INDEX IF NOT EXISTS identify_log_at ON identify_log(sent_at);"
								   "CREATE TABLE IF NOT EXISTS rest_log("
								   "id INTEGER PRIMARY KEY, token TEXT NOT NULL, sent_at INTEGER NOT NULL);"
								   "CREATE INDEX IF NOT EXISTS rest_log_token_at ON rest_log(token,sent_at);"
								   "CREATE INDEX IF NOT EXISTS rest_log_at ON rest_log(sent_at);"
								   "CREATE TABLE IF NOT EXISTS rest_route("
								   "token TEXT NOT NULL, route TEXT NOT NULL, bucket TEXT NOT NULL, seen_at INTEGER NOT NULL,"
								   "PRIMARY KEY(token,route)) WITHOUT ROWID;"
								   "CREATE INDEX IF NOT EXISTS rest_route_seen ON rest_route(token,seen_at);"
								   "CREATE TABLE IF NOT EXISTS rest_reset("
								   "token TEXT NOT NULL, key TEXT NOT NULL, reset_at INTEGER NOT NULL,"
								   "PRIMARY KEY(token,key)) WITHOUT ROWID;"
								   "CREATE INDEX IF NOT EXISTS rest_reset_at ON rest_reset(reset_at);"
								   "CREATE TABLE IF NOT EXISTS rest_global("
								   "token TEXT PRIMARY KEY, reset_at INTEGER NOT NULL) WITHOUT ROWID;"
								   "CREATE TABLE IF NOT EXISTS invalid_log("
								   "id INTEGER PRIMARY KEY, sent_at INTEGER NOT NULL, invalid INTEGER NOT NULL);"
								   "CREATE INDEX IF NOT EXISTS invalid_log_at ON invalid_log(sent_at);");
		ready.store(prepared, std::memory_order_release);
		return prepared;
	}

	// SQLを実行し、成功したか返す。
	bool exec(const char *p_sql) {
		return db && sqlite3_exec(db, p_sql, nullptr, nullptr, nullptr) == SQLITE_OK;
	}

	// 生のSQLite接続をstatementへ渡す。
	sqlite3 *get() const {
		return db;
	}

	// 開いたSQLite接続を閉じる。
	~Db() {
		if (db) {
			sqlite3_close(db);
		}
	}
};

// prepared statementをscope終了時に必ず破棄する。
class Stmt {
	sqlite3_stmt *stmt = nullptr; // 現在のprepared statement

public:
	// SQLをprepareして使用可能か返す。
	bool open(sqlite3 *p_db, const char *p_sql) {
		return sqlite3_prepare_v2(p_db, p_sql, -1, &stmt, nullptr) == SQLITE_OK;
	}

	// token digestを指定位置へbindする。
	bool text(int p_at, const CharString &p_value) {
		return sqlite3_bind_text(stmt, p_at, p_value.get_data(), p_value.length(), SQLITE_TRANSIENT) == SQLITE_OK;
	}

	// 64bit整数を指定位置へbindする。
	bool integer(int p_at, int64_t p_value) {
		return sqlite3_bind_int64(stmt, p_at, p_value) == SQLITE_OK;
	}

	// statementを1段進める。
	int step() {
		return sqlite3_step(stmt);
	}

	// 結果列を64bit整数で読む。
	int64_t column(int p_at) const {
		return sqlite3_column_int64(stmt, p_at);
	}

	// 結果列をUTF-8文字列で読む。
	String string(int p_at) const {
		const char *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt, p_at));
		return value ? String::utf8(value) : String();
	}

	// prepared statementを破棄する。
	~Stmt() {
		if (stmt) {
			sqlite3_finalize(stmt);
		}
	}
};

// 永続化できるUnix時刻をmsで返す。
int64_t wall_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// transactionを失敗として閉じる。
IdentifyStore::Gate fail(Db &p_db) {
	p_db.exec("ROLLBACK;");
	return {};
}

// REST判定を失敗としてtransactionを閉じる。
RestStore::Gate rest_fail(Db &p_db) {
	p_db.exec("ROLLBACK;");
	return {};
}

// RESTを待機としてtransactionを閉じる。
RestStore::Gate rest_wait(Db &p_db, int64_t p_now, int64_t p_until) {
	p_db.exec("ROLLBACK;");
	RestStore::Gate out;
	out.result = RestStore::WAIT;
	out.wait_ms = uint64_t(MAX(int64_t(1), p_until - p_now));
	return out;
}

} // namespace

// Gateway Bot APIの公式残数とrolling上限を共有DBへ反映する。
bool IdentifyStore::sync(const String &p_token, int p_remaining, uint64_t p_reset_ms, int p_limit) {
	Db db;
	if (!db.open() || !db.exec("BEGIN IMMEDIATE;")) {
		return false;
	}
	const int64_t now = wall_ms();
	const int64_t reset_at = now + int64_t(p_reset_ms);
	const CharString token = p_token.utf8();
	Stmt current;
	if (!current.open(db.get(), "SELECT reset_at, remaining, next_at, cap FROM identify_budget WHERE token=?1;") || !current.text(1, token)) {
		db.exec("ROLLBACK;");
		return false;
	}
	const int row = current.step();
	int64_t next_at = 0;
	int remaining = p_remaining;
	int limit = p_limit;
	int64_t until = reset_at;
	if (row == SQLITE_ROW) {
		next_at = current.column(2);
		if (current.column(0) > now) {
			until = MAX(current.column(0), reset_at);
			remaining = MIN(int(current.column(1)), p_remaining);
			limit = MIN(int(current.column(3)), p_limit);
		}
	} else if (row != SQLITE_ROW && row != SQLITE_DONE) {
		db.exec("ROLLBACK;");
		return false;
	}
	Stmt save;
	const char *sql = "INSERT INTO identify_budget(token,reset_at,remaining,next_at,cap) VALUES(?1,?2,?3,?4,?5) "
					  "ON CONFLICT(token) DO UPDATE SET reset_at=excluded.reset_at,remaining=excluded.remaining,"
					  "next_at=excluded.next_at,cap=excluded.cap;";
	if (!save.open(db.get(), sql) || !save.text(1, token) || !save.integer(2, until) || !save.integer(3, remaining) ||
			!save.integer(4, next_at) || !save.integer(5, limit) || save.step() != SQLITE_DONE || !db.exec("COMMIT;")) {
		db.exec("ROLLBACK;");
		return false;
	}
	return true;
}

// 上限内ならIdentify 1件を原子的に記録する。
IdentifyStore::Gate IdentifyStore::take(const String &p_token) {
	Db db;
	if (!db.open() || !db.exec("BEGIN IMMEDIATE;")) {
		return {};
	}
	const int64_t now = wall_ms();
	const int64_t cutoff = now - DAY_MS;
	const CharString token = p_token.utf8();
	Stmt clean;
	if (!clean.open(db.get(), "DELETE FROM identify_log WHERE sent_at<=?1;") || !clean.integer(1, cutoff) || clean.step() != SQLITE_DONE) {
		return fail(db);
	}
	Stmt stale;
	if (!stale.open(db.get(), "DELETE FROM identify_budget WHERE reset_at<=?1;") || !stale.integer(1, now) || stale.step() != SQLITE_DONE) {
		return fail(db);
	}
	Stmt budget;
	if (!budget.open(db.get(), "SELECT reset_at,remaining,next_at,cap FROM identify_budget WHERE token=?1;") || !budget.text(1, token)) {
		return fail(db);
	}
	const int budget_row = budget.step();
	if (budget_row == SQLITE_DONE) {
		db.exec("ROLLBACK;");
		Gate out;
		out.result = REFRESH;
		return out;
	}
	if (budget_row != SQLITE_ROW) {
		return fail(db);
	}
	const int64_t reset_at = budget.column(0);
	const int remaining = int(budget.column(1));
	const int64_t next_at = budget.column(2);
	const int limit = int(budget.column(3));
	if (now >= reset_at) {
		db.exec("ROLLBACK;");
		Gate out;
		out.result = REFRESH;
		return out;
	}
	if (remaining <= 0) {
		db.exec("ROLLBACK;");
		Gate out;
		out.result = WAIT;
		out.wait_ms = uint64_t(reset_at - now);
		return out;
	}
	Stmt count;
	if (!count.open(db.get(), "SELECT COUNT(*),MIN(sent_at) FROM identify_log WHERE token=?1;") || !count.text(1, token) || count.step() != SQLITE_ROW) {
		return fail(db);
	}
	if (count.column(0) >= limit) {
		const int64_t release_at = count.column(1) + DAY_MS;
		db.exec("ROLLBACK;");
		Gate out;
		out.result = WAIT;
		out.wait_ms = uint64_t(MAX(int64_t(1), release_at - now));
		return out;
	}
	if (now < next_at) {
		db.exec("ROLLBACK;");
		Gate out;
		out.result = WAIT;
		out.wait_ms = uint64_t(next_at - now);
		return out;
	}
	Stmt add;
	if (!add.open(db.get(), "INSERT INTO identify_log(token,sent_at) VALUES(?1,?2);") || !add.text(1, token) ||
			!add.integer(2, now) || add.step() != SQLITE_DONE) {
		return fail(db);
	}
	Stmt use;
	if (!use.open(db.get(), "UPDATE identify_budget SET remaining=remaining-1,next_at=?2 WHERE token=?1;") || !use.text(1, token) ||
			!use.integer(2, now + IDENTIFY_INTERVAL) || use.step() != SQLITE_DONE || !db.exec("COMMIT;")) {
		return fail(db);
	}
	Gate out;
	out.result = GRANTED;
	return out;
}

// invalid、global、route bucketの上限内ならREST 1件を原子的に予約する。
RestStore::Gate rest_take(const String &p_token, const String &p_route, const String &p_major, int p_invalid_limit) {
	Db db;
	if (!db.open(false, REST_SQLITE_WAIT) || !db.exec("BEGIN IMMEDIATE;")) {
		return {};
	}
	const int64_t now = wall_ms();
	const CharString token = p_token.utf8();
	const CharString route = p_route.utf8();
	const String major = p_major;
	Stmt clean_invalid;
	if (!clean_invalid.open(db.get(), "DELETE FROM invalid_log WHERE sent_at<=?1;") ||
			!clean_invalid.integer(1, now - INVALID_WINDOW_MS) || clean_invalid.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt invalid;
	if (!invalid.open(db.get(), "SELECT COUNT(*),MIN(CASE WHEN invalid=1 THEN sent_at END),SUM(invalid) FROM invalid_log;") ||
			invalid.step() != SQLITE_ROW) {
		return rest_fail(db);
	}
	if (invalid.column(0) >= p_invalid_limit) {
		const int64_t until = invalid.column(2) >= p_invalid_limit ? invalid.column(1) + INVALID_WINDOW_MS : now + INVALID_RECHECK_MS;
		return rest_wait(db, now, until);
	}
	Stmt clean_log;
	if (!clean_log.open(db.get(), "DELETE FROM rest_log WHERE sent_at<=?1;") || !clean_log.integer(1, now - REST_RESERVE_MS) ||
			clean_log.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt clean_reset;
	if (!clean_reset.open(db.get(), "DELETE FROM rest_reset WHERE reset_at<=?1;") || !clean_reset.integer(1, now) ||
			clean_reset.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt clean_global;
	if (!clean_global.open(db.get(), "DELETE FROM rest_global WHERE reset_at<=?1;") || !clean_global.integer(1, now) ||
			clean_global.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt clean_route;
	if (!clean_route.open(db.get(), "DELETE FROM rest_route WHERE seen_at<=?1;") || !clean_route.integer(1, now - DAY_MS) ||
			clean_route.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt global;
	if (!global.open(db.get(), "SELECT reset_at FROM rest_global WHERE token=?1;") || !global.text(1, token)) {
		return rest_fail(db);
	}
	const int global_row = global.step();
	if (global_row == SQLITE_ROW) {
		return rest_wait(db, now, global.column(0));
	}
	if (global_row != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt mapped;
	if (!mapped.open(db.get(), "SELECT bucket FROM rest_route WHERE token=?1 AND route=?2;") || !mapped.text(1, token) ||
			!mapped.text(2, route)) {
		return rest_fail(db);
	}
	const int mapped_row = mapped.step();
	if (mapped_row != SQLITE_ROW && mapped_row != SQLITE_DONE) {
		return rest_fail(db);
	}
	const String bucket = mapped_row == SQLITE_ROW ? mapped.string(0) : String();
	const String key = bucket.is_empty() ? String::utf8(route.get_data()) : bucket + String(":") + major;
	Stmt reset;
	if (!reset.open(db.get(), "SELECT reset_at FROM rest_reset WHERE token=?1 AND key=?2;") || !reset.text(1, token) ||
			!reset.text(2, key.utf8())) {
		return rest_fail(db);
	}
	const int reset_row = reset.step();
	if (reset_row == SQLITE_ROW) {
		return rest_wait(db, now, reset.column(0));
	}
	if (reset_row != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt count;
	if (!count.open(db.get(), "SELECT COUNT(*),MIN(sent_at) FROM rest_log WHERE token=?1;") || !count.text(1, token) ||
			count.step() != SQLITE_ROW) {
		return rest_fail(db);
	}
	if (count.column(0) >= REST_GLOBAL_SAFE) {
		return rest_wait(db, now, count.column(1) + REST_RESERVE_MS);
	}
	Stmt add;
	if (!add.open(db.get(), "INSERT INTO rest_log(token,sent_at) VALUES(?1,?2);") || !add.text(1, token) ||
			!add.integer(2, now) || add.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	Stmt reserve;
	if (!reserve.open(db.get(), "INSERT INTO invalid_log(sent_at,invalid) VALUES(?1,0);") || !reserve.integer(1, now) || reserve.step() != SQLITE_DONE) {
		return rest_fail(db);
	}
	const uint64_t permit = uint64_t(sqlite3_last_insert_rowid(db.get()));
	if (!db.exec("COMMIT;")) {
		return rest_fail(db);
	}
	RestStore::Gate out;
	out.result = RestStore::GRANTED;
	out.grant_until = uint64_t(now + REST_GRANT_MS);
	out.permit = permit;
	out.bucket = bucket;
	return out;
}

// Discord応答のbucketと再開までの時間を共有DBへ反映する。
bool rest_sync(const String &p_token, const String &p_route, const String &p_major, const String &p_bucket,
		uint64_t p_wait_ms, bool p_global, uint64_t p_permit, bool p_invalid, int p_db_wait) {
	Db db;
	if (!db.open(false, p_db_wait) || !db.exec("BEGIN IMMEDIATE;")) {
		return false;
	}
	const int64_t now = wall_ms();
	const CharString token = p_token.utf8();
	const CharString route_key = p_route.utf8();
	const String major = p_major;
	if (p_permit > 0) {
		Stmt finish;
		const char *sql = p_invalid ? "UPDATE invalid_log SET invalid=1 WHERE id=?1;" : "DELETE FROM invalid_log WHERE id=?1;";
		if (!finish.open(db.get(), sql) || !finish.integer(1, int64_t(p_permit)) || finish.step() != SQLITE_DONE) {
			rest_fail(db);
			return false;
		}
	}
	if (!p_bucket.is_empty()) {
		Stmt route;
		const char *sql = "INSERT INTO rest_route(token,route,bucket,seen_at) VALUES(?1,?2,?3,?4) "
						  "ON CONFLICT(token,route) DO UPDATE SET bucket=excluded.bucket,seen_at=excluded.seen_at;";
		if (!route.open(db.get(), sql) || !route.text(1, token) || !route.text(2, route_key) ||
				!route.text(3, p_bucket.utf8()) || !route.integer(4, now) || route.step() != SQLITE_DONE) {
			rest_fail(db);
			return false;
		}
		Stmt count;
		if (!count.open(db.get(), "SELECT COUNT(*) FROM rest_route;") || count.step() != SQLITE_ROW) {
			rest_fail(db);
			return false;
		}
		if (count.column(0) > REST_ROUTE_MAX) {
			Stmt trim;
			const char *trim_sql = "DELETE FROM rest_route WHERE (token,route)=("
								   "SELECT token,route FROM rest_route ORDER BY seen_at ASC LIMIT 1);";
			if (!trim.open(db.get(), trim_sql) || trim.step() != SQLITE_DONE) {
				rest_fail(db);
				return false;
			}
		}
	}
	if (p_wait_ms > 0) {
		const int64_t until = now + int64_t(p_wait_ms);
		Stmt stop;
		const char *sql = p_global ? "INSERT INTO rest_global(token,reset_at) VALUES(?1,?2) ON CONFLICT(token) DO UPDATE SET reset_at=MAX(reset_at,excluded.reset_at);" : "INSERT INTO rest_reset(token,key,reset_at) VALUES(?1,?2,?3) ON CONFLICT(token,key) DO UPDATE SET reset_at=MAX(reset_at,excluded.reset_at);";
		const String key = p_bucket.is_empty() ? String::utf8(route_key.get_data()) : p_bucket + String(":") + major;
		if (!stop.open(db.get(), sql) || !stop.text(1, token) ||
				(p_global ? !stop.integer(2, until) : (!stop.text(2, key.utf8()) || !stop.integer(3, until))) ||
				stop.step() != SQLITE_DONE) {
			rest_fail(db);
			return false;
		}
	}
	if (!db.exec("COMMIT;")) {
		db.exec("ROLLBACK;");
		return false;
	}
	return true;
}

namespace {

// SQLite workerに渡す1件のREST判定または応答更新。
struct RestJob {
	enum Type {
		TAKE, // 送信前の上限判定
		SYNC, // 応答のbucketと待機更新
	};

	Type type = TAKE; // workerが行う処理
	uint64_t id = 0; // TAKE結果をpollするID
	String token; // Bot token digest
	String route; // 正規化route
	String major; // 主要resource
	String bucket; // Discord bucket
	uint64_t wait_ms = 0; // 再開までのms
	uint64_t epoch = 0; // queue追加時のtoken制限世代
	uint64_t permit = 0; // invalid応答枠の予約ID
	int invalid_limit = 900; // 10分のinvalid応答上限
	bool global = false; // global停止か
	bool invalid = false; // invalid応答として予約を残すか
};

// process内のREST SQLite処理を1本のbackground threadへ集約する。
class RestWorker {
	std::mutex mutex; // mainとworker間のqueue保護
	std::condition_variable wake; // 新しいjobまたは終了を通知する
	std::deque<RestJob> jobs; // 受付順のSQLite job
	std::unordered_map<uint64_t, RestStore::Gate> results; // mainが未取得の判定結果
	std::unordered_set<uint64_t> pending; // worker処理前または処理中のID
	std::unordered_set<uint64_t> cancelled; // 完了後も結果を保持しないID
	std::unordered_set<std::string> blocked; // 応答更新に失敗したtoken
	std::thread runner; // SQLiteだけを実行するworker
	uint64_t next_id = 1; // 次のTAKE ID
	uint64_t epoch = 0; // 制限応答ごとに進むpermit世代
	size_t sync_pending = 0; // queue内の応答更新数
	std::atomic_bool stopping = false; // 新規jobを受けない状態

	// token digestをstandard containerのkeyへ直す。
	static std::string key_of(const String &p_token) {
		const CharString key = p_token.utf8();
		return std::string(key.get_data(), key.length());
	}

	// main側で未送信になったpermitの解放をqueueへ積む。
	void release_locked(uint64_t p_permit) {
		if (p_permit == 0 || sync_pending >= REST_SYNC_MAX) {
			return;
		}
		RestJob job;
		job.type = RestJob::SYNC;
		job.permit = p_permit;
		jobs.push_back(job);
		sync_pending++;
		wake.notify_one();
	}

	// queueを順番に処理し、TAKEだけ結果をmainへ渡す。
	void run() {
		while (true) {
			RestJob job;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [this] { return stopping.load() || !jobs.empty(); });
				if (jobs.empty()) {
					return;
				}
				job = jobs.front();
				jobs.pop_front();
				if (job.type == RestJob::SYNC) {
					sync_pending--;
				}
			}
			if (job.type == RestJob::SYNC) {
				const int db_wait = stopping.load() ? 1 : REST_SQLITE_WAIT;
				if (!rest_sync(job.token, job.route, job.major, job.bucket, job.wait_ms, job.global, job.permit, job.invalid, db_wait)) {
					std::lock_guard<std::mutex> lock(mutex);
					blocked.insert(key_of(job.token));
				}
				continue;
			}
			RestStore::Gate gate;
			bool denied = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				denied = blocked.find(key_of(job.token)) != blocked.end();
			}
			if (denied) {
				gate.result = RestStore::FAILED;
			} else {
				gate = rest_take(job.token, job.route, job.major, job.invalid_limit);
			}
			gate.epoch = job.epoch;
			bool discard = false;
			{
				std::lock_guard<std::mutex> lock(mutex);
				pending.erase(job.id);
				discard = cancelled.erase(job.id) > 0 || stopping.load();
				if (!discard) {
					results.insert_or_assign(job.id, gate);
				}
			}
			if (discard && gate.result == RestStore::GRANTED) {
				const int db_wait = stopping.load() ? 1 : REST_SQLITE_WAIT;
				rest_sync(String(), String(), String(), String(), 0, false, gate.permit, false, db_wait);
			}
		}
	}

public:
	// 長寿命workerを最初の利用時に1本だけ始める。
	RestWorker() : runner(&RestWorker::run, this) {}

	// REST送信判定をqueueへ積み、poll用IDを返す。
	uint64_t take(const String &p_token, const String &p_route, const String &p_major, int p_invalid_limit) {
		std::lock_guard<std::mutex> lock(mutex);
		if (stopping.load()) {
			return 0;
		}
		RestJob job;
		job.id = next_id++;
		job.token = p_token;
		job.route = p_route;
		job.major = p_major;
		job.invalid_limit = p_invalid_limit;
		job.epoch = epoch;
		pending.insert(job.id);
		jobs.push_back(job);
		wake.notify_one();
		return job.id;
	}

	// Discord応答の制限情報をTAKEと同じqueueへ積む。
	void sync(RestJob p_job) {
		std::lock_guard<std::mutex> lock(mutex);
		if (!stopping.load()) {
			if (sync_pending >= REST_SYNC_MAX) {
				blocked.insert(key_of(p_job.token));
				return;
			}
			if (p_job.wait_ms > 0) {
				p_job.epoch = ++epoch;
			}
			jobs.push_back(p_job);
			sync_pending++;
			wake.notify_one();
		}
	}

	// 完了済みTAKE結果を1件取り出す。
	bool poll(uint64_t p_id, RestStore::Gate &r_gate) {
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = results.find(p_id);
		if (found == results.end()) {
			return false;
		}
		r_gate = found->second;
		results.erase(found);
		return true;
	}

	// permit以降に新しい制限応答を受けていないか返す。
	bool fresh(const RestStore::Gate &p_gate) {
		std::lock_guard<std::mutex> lock(mutex);
		return epoch == p_gate.epoch;
	}

	// 破棄するTAKEの結果を残さないよう記録する。
	void cancel(uint64_t p_id) {
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = results.find(p_id);
		if (found != results.end()) {
			release_locked(found->second.permit);
			results.erase(found);
		} else if (pending.find(p_id) != pending.end()) {
			cancelled.insert(p_id);
		}
	}

	// 未送信判定を破棄し、応答更新だけ永続化してから止める。
	void stop() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (stopping.load()) {
				return;
			}
			stopping.store(true);
			jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [](const RestJob &p_job) {
				return p_job.type == RestJob::TAKE;
			}), jobs.end());
			results.clear();
			pending.clear();
			cancelled.clear();
		}
		wake.notify_one();
		if (runner.joinable()) {
			runner.join();
		}
	}

	// library解放時にthreadを残さない。
	~RestWorker() {
		stop();
	}
};

// process内で共有する単一SQLite workerを返す。
RestWorker &rest_worker() {
	static RestWorker worker;
	return worker;
}

} // namespace

// REST送信前のSQLite判定をworkerへ予約する。
uint64_t RestStore::take_async(const String &p_token, const String &p_route, const String &p_major, int p_invalid_limit) {
	return rest_worker().take(p_token, p_route.sha256_text(), p_major.sha256_text(), p_invalid_limit);
}

// workerの判定が終わっていれば結果を受け取る。
bool RestStore::poll(uint64_t p_job, Gate &r_gate) {
	return rest_worker().poll(p_job, r_gate);
}

// workerが確保した送信枠がまだ新鮮か返す。
bool RestStore::fresh(const Gate &p_gate) {
	return p_gate.result == GRANTED && uint64_t(wall_ms()) <= p_gate.grant_until && rest_worker().fresh(p_gate);
}

// 不要になった判定を破棄する。
void RestStore::cancel(uint64_t p_job) {
	if (p_job > 0) {
		rest_worker().cancel(p_job);
	}
}

// Discord応答のbucketと再開までの時間をworkerから反映する。
void RestStore::sync_async(const String &p_token, const String &p_route, const String &p_major, const String &p_bucket,
		uint64_t p_wait_ms, bool p_global, uint64_t p_permit, bool p_invalid) {
	RestJob job;
	job.type = RestJob::SYNC;
	job.token = p_token;
	job.route = p_route.sha256_text();
	job.major = p_major.sha256_text();
	job.bucket = p_bucket;
	job.wait_ms = p_wait_ms;
	job.permit = p_permit;
	job.global = p_global;
	job.invalid = p_invalid;
	rest_worker().sync(job);
}

// 送信前に失効したinvalid応答枠を解放する。
void RestStore::release_async(uint64_t p_permit) {
	if (p_permit > 0) {
		sync_async(String(), String(), String(), String(), 0, false, p_permit, false);
	}
}

// GDExtension解放前にworkerを安全に終了する。
void RestStore::shutdown() {
	rest_worker().stop();
}

} // namespace godot
