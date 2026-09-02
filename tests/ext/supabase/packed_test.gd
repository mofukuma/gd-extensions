# compile成果物が同梱GDExtensionを起動時に読めるか確かめる。
extends RefCounted


# Supabase Singletonと内部clientの生成結果を終了値へ反映する。
func main() -> int:
	if not Engine.has_singleton("GDSupabase"):
		return 1
	var sb := GDSupabase.client("https://example.supabase.co", "sb_publishable_test")
	print("packed_supabase=", sb != null)
	return 0 if sb != null else 2
