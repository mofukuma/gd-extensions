# Supabase GDExtension

SupabaseのDatabaseとAuthをgdから使う。公開名は`GDSupabase`と`GDSupabaseClient`で、clientの型名は通常書かない。

```sh
gd add ext:@mofukuma/supabase@^0.1.1
```

```gdscript
var sb := GDSupabase.client(
	GD.cli.env("SUPABASE_URL"),
	GD.cli.env("SUPABASE_PUBLISHABLE_KEY"),
)

func recent_posts():
	var res := await sb.select("posts", {
		"select": "id,title,made_at",
		"order": "id.desc",
		"limit": 20,
	})
	if not res.ok:
		return [], res.error
	return res.data, null
```

Databaseは`select`、`insert`、`update`、`remove`、`rpc`、Authは`auth_settings`、`sign_in`、
`refresh`、`sign_out`、`session`を持つ。Realtime、Storage、OAuth redirect、session永続化は含まない。

## build

Godot 4.7対応のgodot-cpp v10を用意してbuildする。

```sh
git clone https://github.com/godotengine/godot-cpp tmp/ref_godot_cpp
git -C tmp/ref_godot_cpp checkout 9c8aeff0f58ad030f3d1030e8262de1322cd0ccd
scons -C extensions/supabase godot_cpp=../../tmp/ref_godot_cpp
```

projectへ`supabase.gdextension`と`bin/`を置き、通常はextension一覧から起動時に読む。
実行中に読む場合は次の形になる。

```gdscript
GDExtensionManager.load_extension("res://supabase.gdextension")
var api := Engine.get_singleton("GDSupabase")
var sb := api.client(url, key)
```

`sb_secret_` keyと旧`service_role` JWTは拒否する。配布物にはpublishable keyだけを渡し、権限はSupabaseのRLSで制御する。
