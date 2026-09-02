/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Supabaseの内部classと公開SingletonをGDExtensionへ登録する。

#include "register_types.h"

#include "supabase_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static GDSupabase *supabase_api = nullptr; // 公開Singletonの実体

// SupabaseのclassとSingletonを登録する。
void initialize_supabase_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_INTERNAL_CLASS(GDSupabaseCallInternal);
	GDREGISTER_CLASS(GDSupabaseClient);
	GDREGISTER_CLASS(GDSupabase);
	supabase_api = memnew(GDSupabase);
	Engine::get_singleton()->register_singleton("GDSupabase", supabase_api);
}

// SupabaseのSingletonを外してclassを解放可能にする。
void uninitialize_supabase_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	Engine::get_singleton()->unregister_singleton("GDSupabase");
	memdelete(supabase_api);
	supabase_api = nullptr;
}

// engineから呼ばれるGDExtension初期化入口。
extern "C" GDExtensionBool GDE_EXPORT supabase_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init(p_get_proc_address, p_library, r_initialization);
	init.register_initializer(initialize_supabase_module);
	init.register_terminator(uninitialize_supabase_module);
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init.init();
}
