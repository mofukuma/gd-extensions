# Windows版三拡張を本家Godotで起動して読込みを確かめる。
param(
    [Parameter(Mandatory = $true)][string]$Godot,
    [string]$Target = "template_debug"
)
$ErrorActionPreference = "Stop"

# manifestとdebug libraryを試験projectへ置いて起動する。
foreach ($name in @("discord", "memcached", "supabase")) {
    $project = "tmp/${name}_windows_test"
    New-Item -ItemType Directory -Force "$project/bin", "$project/.godot" | Out-Null
    Copy-Item "tests/ext/$name/project.godot" "$project/project.godot"
    Copy-Item "tests/ext/startup_smoke.gd" "$project/startup_smoke.gd"
    Copy-Item "tests/ext/$name/extension_list.cfg" "$project/.godot/extension_list.cfg"
    Copy-Item "extensions/$name/${name}.gdextension" "$project/"
    # editorのdebug選択先をreleaseへ向け、release library自体を起動する。
    if ($Target -eq "template_release") {
        $manifest = "$project/${name}.gdextension"
        (Get-Content $manifest -Raw -Encoding utf8).Replace("template_debug", "template_release") | Set-Content $manifest -Encoding utf8
    }
    Copy-Item "tmp/${name}_ext_bin/libgd${name}.windows.${Target}.x86_64.dll" "$project/bin/"
    $env:GD_EXT_SINGLETON = "GD" + (Get-Culture).TextInfo.ToTitleCase($name)
    & $Godot --headless --path $project --script res://startup_smoke.gd
    if ($LASTEXITCODE -ne 0) { throw "$name failed to start" }
}
