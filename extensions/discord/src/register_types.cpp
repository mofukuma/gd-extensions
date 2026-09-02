/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Discord Botの内部classと公開SingletonをGDExtensionへ登録する。

#include "register_types.h"

#include "discord_client.h"
#include "identify_store.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static GDDiscord *discord_api = nullptr; // 公開Singletonの実体

// DiscordのclassとSingletonを登録する。
void initialize_discord_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_INTERNAL_CLASS(GDDiscordCallInternal);
	GDREGISTER_INTERNAL_CLASS(GDDiscordWireInternal);
	GDREGISTER_CLASS(GDDiscordClient);
	GDREGISTER_CLASS(GDDiscord);
	discord_api = memnew(GDDiscord);
	Engine::get_singleton()->register_singleton("GDDiscord", discord_api);
}

// DiscordのSingletonを外してclassを解放可能にする。
void uninitialize_discord_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	Engine::get_singleton()->unregister_singleton("GDDiscord");
	memdelete(discord_api);
	discord_api = nullptr;
	GDDiscordRestStore::shutdown();
}

// engineから呼ばれるGDExtension初期化入口。
extern "C" GDExtensionBool GDE_EXPORT discord_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init(p_get_proc_address, p_library, r_initialization);
	init.register_initializer(initialize_discord_module);
	init.register_terminator(uninitialize_discord_module);
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init.init();
}
