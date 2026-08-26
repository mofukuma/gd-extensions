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
rate limitはSQLiteで同一machine・OS user上の同じBot tokenのclient・project・process間に共有する。

`set_presence({"since": null, "activities": [], "status": "online", "afk": false})`で
presenceを更新できる。連続更新は最新値へまとめ、過去20秒で5回までにする。
RESTは安全側の直列実行で、Discord bucketとglobal制限を待つ。待ちbody総量は既定16 MiBまで。
401、403、共有範囲外の429は全Bot共通で10分900件までに制限し、401後は同じclientから送らない。
通信中に閉じて応答を確認できなかった要求も、安全側でinvalid枠へ残す。
Stringのmessageはmentionを発火させない。mentionが必要な場合はDictionaryの`allowed_mentions`で明示する。
`api_url`の上書きはlocalhost試験だけに制限される。Gateway URLは`GET /gateway/bot`から取得する。
Gateway packetと1frameは既定2 MiBまで。`max_gateway_packet`は64 KiBから8 MiBの範囲で設定できる。
IdentifyとREST制限はPOSIXの`$HOME/.gd_cli_discord_limits.sqlite3`、Windowsの`LOCALAPPDATA`へ
token digestと送信時刻を保存する。Identifyは同じOS userのprojectとprocessをまたいで過去24時間900件まで、
RESTは非同期判定の遅延も含め、安全幅を持たせた1.1秒45件までにする。
複数machineで同じtokenを使う場合はSQLiteで共有できないため、1台のREST proxy経由にする。
`identify_limit`で変更でき、Discord公式上限以上にはならない。
`invalid_limit`は1から900の範囲で既定上限をさらに小さくできる。

tokenは`.env`の`DISCORD_TOKEN`から渡し、source、`gd.json`、配布物へ入れない。
strictではDiscordと環境変数だけを許可する。

```sh
gd --strict --allow-env=DISCORD_TOKEN \
	--allow-net=discord.com:443,*.discord.gg:443 --allow-ext serve bot.gd
```

## 通信module

GatewayにはGodot本家の`WebSocketPeer`をそのまま使う。別のWebSocket実装は不要。
文字BotはWebRTCを使わない。WebRTCが必要な別機能では、本家公式
[`godotengine/webrtc-native`](https://github.com/godotengine/webrtc-native)を使う。
Discord VoiceはWebRTCではなく、専用WebSocket、UDP/RTP、Opus、DAVEが必要なため対象外。

## build

Godot 4.7対応のgodot-cpp v10を用意してbuildする。SQLiteはextensionに収録される。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
scons -C extensions/discord godot_cpp=../../tmp/ref_godot_cpp out=../../tmp/discord_ext_bin
```

projectへ`discord.gdextension`と対象platformの`bin/`を置き、extension一覧から起動時に読む。
