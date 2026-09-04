# installしたDiscord純GDScript packageを明示preloadで確かめる。
extends SceneTree

const Discord := preload("res://vendor/discord/mod.gd") # install済みmodule


# main loop開始後に公開API試験を予約する。
func _init():
	call_deferred("run")


# 公開定数とfactoryを使って終了する。
func run():
	var bot = Discord.bot("smoke-token", {"api_url": "http://127.0.0.1:9/api/v10"})
	var ok := Discord.GUILDS == 1 and Discord.MESSAGE_CONTENT == 32768 and bot != null
	if bot != null:
		bot.close()
	print("discord_package=", ok)
	quit(0 if ok else 1)
