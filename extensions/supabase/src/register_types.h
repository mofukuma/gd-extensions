/**************************************************************************/
/*  register_types.h                                                      */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

#pragma once

// Supabase GDExtensionの登録入口を宣言する。

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>

// SupabaseのclassとSingletonを登録する。
void initialize_supabase_module(godot::ModuleInitializationLevel p_level);
// SupabaseのSingletonを外してclassを解放可能にする。
void uninitialize_supabase_module(godot::ModuleInitializationLevel p_level);

// engineから呼ばれるGDExtension初期化入口。
extern "C" GDExtensionBool GDE_EXPORT supabase_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization);
