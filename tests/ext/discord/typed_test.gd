# Discord extensionの内部型とawait結果をstrictで推論できるか確かめる。
extends Node


# 公開factory、状態、非同期RESTの型を使う。
func probe() -> void:
	var bot := GDDiscord.bot("test", {"api_url": "http://127.0.0.1:9"})
	var _ready: bool = bot.is_ready()
	var _presence: Error = bot.set_presence({"since": null, "activities": [], "status": "online", "afk": false})
	var reply: Dictionary = await bot.send_message("123", "hello")
	var _ok: bool = reply.ok
	bot.close()
