# Discord Bot

Discord Gateway v10とRESTを使う純GDScript文字Bot packageです。C++、GDExtension、platform別binaryは使いません。

```sh
gd add discord gd:@mofukuma/discord@^0.1.2
```

```gdscript
const Discord := preload("res://vendor/discord/mod.gd")

var bot


# Botを起動し、Gatewayの準備完了を待つ。
func main():
	var token := OS.get_environment("DISCORD_TOKEN")
	bot = Discord.bot(token, {
		"intents": Discord.GUILDS | Discord.GUILD_MESSAGES | Discord.MESSAGE_CONTENT,
	})
	if bot == null:
		return 1
	bot.event.connect(on_event)
	if bot.start() != OK:
		return 1
	await bot.ready
	return 0


# `!ping`へ返信する。
func on_event(name, data):
	if name == "MESSAGE_CREATE" and data.content == "!ping":
		var reply := await bot.send_message(data.channel_id, "pong")
		if not reply.ok:
			printerr(reply.error)
```

Gatewayは`ready`、`event`、`resumed`、`disconnected`、`failed` signalを出します。
RESTは`request`、`send_message`、`edit_message`、`delete_message`を持ち、結果は
`{ok, status, data, error, headers}`です。heartbeat、Resume、429待機、globalとrouteの
rate limitを処理します。

`set_presence({"since": null, "activities": [], "status": "online", "afk": false})`で
presenceを更新できます。連続更新は最新値へまとめ、過去20秒で5回までにします。
RESTは直列実行し、待ちbody総量は既定16 MiBまでです。Stringのmessageはmentionを発火させません。
mentionが必要ならDictionaryの`allowed_mentions`で明示します。

一つのprocess内ではtokenごとのglobal・bucket・Identify制限を全clientで共有します。
純GDScriptにはprocess間を排他的に更新するportableなfile lockがないため、複数processや複数machineで
同じtokenを使う構成は、一つのBot processへ集約するかREST proxyを使ってください。

tokenは`.env`の`DISCORD_TOKEN`から渡し、source、`gd.json`、配布物へ入れません。
strictではDiscordと環境変数だけを許可します。

```sh
gd --strict --allow-env=DISCORD_TOKEN \
	--allow-net=discord.com:443,*.discord.gg:443 serve bot.gd
```

GatewayにはGodot本家の`WebSocketPeer`、RESTには`HTTPRequest`を使います。
Discord Voiceは専用WebSocket、UDP/RTP、Opus、DAVEが必要なため対象外です。

## package開発

公開入口は`src/mod.gd`です。利用側と同じく明示preloadで試験します。

```sh
gd check extensions/discord/src/mod.gd
gd fmt --check extensions/discord/src/mod.gd
GODOT="/Applications/Godot 4.7.1.app/Contents/MacOS/Godot" bash tests/discord.sh
```
