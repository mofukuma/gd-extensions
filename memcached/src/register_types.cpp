/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

// Memcachedの内部classと公開SingletonをGDExtensionへ登録する。

#include "register_types.h"

#include "memcached_client.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static Memcached *memcached_api = nullptr; // 公開Singletonの実体

// MemcachedのclassとSingletonを登録する。
void initialize_memcached_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_INTERNAL_CLASS(MemcachedCallInternal);
	GDREGISTER_INTERNAL_CLASS(MemcachedWireInternal);
	GDREGISTER_CLASS(MemcachedClient);
	GDREGISTER_CLASS(Memcached);
	memcached_api = memnew(Memcached);
	Engine::get_singleton()->register_singleton("Memcached", memcached_api);
}

// MemcachedのSingletonを外してclassを解放可能にする。
void uninitialize_memcached_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	Engine::get_singleton()->unregister_singleton("Memcached");
	memdelete(memcached_api);
	memcached_api = nullptr;
}

// engineから呼ばれるGDExtension初期化入口。
extern "C" GDExtensionBool GDE_EXPORT memcached_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init(p_get_proc_address, p_library, r_initialization);
	init.register_initializer(initialize_memcached_module);
	init.register_terminator(uninitialize_memcached_module);
	init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init.init();
}
