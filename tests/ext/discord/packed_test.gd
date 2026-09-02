# compileした単一実行体からDiscord extensionを読めるか確かめる。
extends RefCounted


# Singletonと主要intent定数を確認する。
func main() -> int:
	var ok := Engine.has_singleton("GDDiscord") and GDDiscord.GUILDS == 1 and GDDiscord.MESSAGE_CONTENT == 32768
	print("packed_discord=", ok)
	return 0 if ok else 1
