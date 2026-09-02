# Memcached clientとawait結果を型名なしで推論できるか調べる。
extends SceneTree

var cache := GDMemcached.client("127.0.0.1", 11211) # 公開Singletonから推論するclient


# await後のDictionary型とfield accessをstrictで検査する。
func inspect_result() -> bool:
	var res := await cache.get("post:1")
	var ok: bool = res.ok
	return ok
