# 公開キーで実SupabaseのAuth設定へ接続する手動試験。
extends SceneTree


# SceneTree開始後に非同期通信を始める。
func _initialize():
	call_deferred("run")


# 秘密値や応答内容を表示せず、接続結果だけを返す。
func run():
	var url := OS.get_environment("SUPABASE_URL")
	var key := OS.get_environment("SUPABASE_PUBLISHABLE_KEY")
	if url.is_empty() or key.is_empty():
		print("live_supabase=false status=0")
		quit(2)
		return
	var sb := GDSupabase.client(url, key)
	var res: Dictionary = await sb.auth_settings()
	print("live_supabase=%s status=%d" % [res.ok, res.status])
	quit(0 if res.ok else 1)
