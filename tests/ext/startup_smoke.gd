# 起動時にmanifestが選んだnative libraryとSingletonを読めるか確かめる。
extends SceneTree


# 二つのうち指定されたSingletonが登録済みなら正常終了する。
func _initialize() -> void:
	var name := OS.get_environment("GD_EXT_SINGLETON")
	var ok := not name.is_empty() and Engine.has_singleton(name)
	print("extension_startup=", name, " ok=", ok)
	quit(0 if ok else 1)
