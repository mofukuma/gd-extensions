# GDScript型推論

Godot本家では`:=`は型付き推論であり、右辺が動的な`Variant`ならparse errorになる。
型注釈を使わない公開moduleでは、動的な関数戻り値へ`var x = value`を使う。

根拠: https://docs.godotengine.org/en/latest/tutorials/scripting/gdscript/gdscript_basics.html
