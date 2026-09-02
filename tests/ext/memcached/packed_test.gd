# compile成果物が同梱GDExtensionを起動時に読めるか確かめる。
extends RefCounted


# Memcached Singletonとclient生成結果を終了値へ反映する。
func main() -> int:
	if not Engine.has_singleton("GDMemcached"):
		return 1
	var cache := GDMemcached.client("127.0.0.1", 11211)
	print("packed_memcached=", cache != null)
	if cache != null:
		cache.close()
	return 0 if cache != null else 2
