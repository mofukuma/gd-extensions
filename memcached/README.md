# Memcached GDExtension

Memcachedのcache操作をgdと本家Godotから使う。公開名は`GDMemcached`と`GDMemcachedClient`で、clientの型名は通常書かない。

```gdscript
var cache := GDMemcached.client("127.0.0.1", 11211, {"prefix": "blog:"})

func load_post(id):
	var res := await cache.get("post:%s" % id)
	if not res.ok:
		return null, res.msg
	if not res.hit:
		return null, null
	return res.value, null
```

`get`、`get_many`、`set`、`set_raw`、`add`、`replace`、`remove`、`touch`、
`increment`、`decrement`、`version`、`close`を持つ。TCP接続はclient内で再利用する。
待ちpayloadは既定16 MiBまでで、`max_queue_bytes`から調整できる。
名前解決は非同期で、必要なら`ip_type`を`ipv4`または`ipv6`へ固定できる。

String、int、JSON対応値、`PackedByteArray`を保存できる。未知flagsのvalueはraw bytesとして返す。
cache missは`ok=true, hit=false`、通信やprotocolの失敗は`ok=false`になる。
送信後に更新操作がtimeoutした場合は、serverで反映済みか判断できないため`kind="ambiguous"`を返す。
TTLは秒数で、30日を越す値はMemcached仕様どおりUnix時刻として扱われる。

## build

Godot 4.7対応のgodot-cppを用意してbuildする。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
scons -C extensions/memcached godot_cpp=../../tmp/ref_godot_cpp
```

Memcachedはlocalhostまたはprivate networkへ置く。公開networkへ直接出さず、UDPも有効にしない。
