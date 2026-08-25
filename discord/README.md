# Discord Bot GDExtension

Discord Gateway v10とRESTを使う文字Bot拡張。`Discord.bot()`が返すclientを保持して使う。

```gdscript
var bot


# Botを起動し、Gatewayの準備完了を待つ。
func main() -> int:
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
func on_event(name: String, data):
	if name == "MESSAGE_CREATE" and data.content == "!ping":
		var reply: Dictionary = await bot.send_message(data.channel_id, "pong")
		if not reply.ok:
			printerr(reply.error)
```

Gatewayは`ready`、`event`、`resumed`、`disconnected`、`failed` signalを出す。
RESTは`request`、`send_message`、`edit_message`、`delete_message`を持ち、結果は
`{ok, status, data, error, headers}`になる。heartbeat、Resume、429待機、globalとrouteの
rate limitはclient内で処理する。

`set_presence({"since": null, "activities": [], "status": "online", "afk": false})`で
presenceを更新できる。連続更新は最新値へまとめ、5回/20秒を越えない。
RESTは安全側の直列実行で、待ちbody総量は既定16 MiBまで。
`api_url`と`gateway_url`の平文上書きはlocalhost試験だけに制限される。

tokenは`.env`の`DISCORD_TOKEN`から渡し、source、`gd.json`、配布物へ入れない。
strictではDiscordと環境変数だけを許可する。

```sh
gd --strict --allow-env=DISCORD_TOKEN \
	--allow-net=discord.com:443,gateway.discord.gg:443 --allow-ext serve bot.gd
```

## 通信module

GatewayにはGodot本家の`WebSocketPeer`をそのまま使う。別のWebSocket実装は不要。
文字BotはWebRTCを使わない。WebRTCが必要な別機能では、本家公式
[`godotengine/webrtc-native`](https://github.com/godotengine/webrtc-native)を使う。
Discord VoiceはWebRTCではなく、専用WebSocket、UDP/RTP、Opus、DAVEが必要なため対象外。

## build

Godot 4.7対応のgodot-cpp v10を用意してbuildする。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
scons -C extensions/discord godot_cpp=../../tmp/ref_godot_cpp
```

projectへ`discord.gdextension`と対象platformの`bin/`を置き、extension一覧から起動時に読む。
