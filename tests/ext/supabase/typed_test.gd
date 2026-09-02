# Supabase内部clientとawait結果を型名なしで推論できるか調べる。
extends SceneTree

var sb := GDSupabase.client("https://example.supabase.co", "sb_publishable_test") # 内部型を推論するclient


# await後のDictionary型とfield accessをstrictで検査する。
func inspect_result() -> bool:
	var res := await sb.select("items")
	var ok: bool = res.ok
	return ok
