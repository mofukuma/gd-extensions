/**************************************************************************/
/*  register_types.h                                                      */
/**************************************************************************/
/*                          gd-cli / GDScript CLI                         */
/**************************************************************************/

#pragma once

// Discord Bot GDExtensionの登録入口を宣言する。

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>

void initialize_discord_module(godot::ModuleInitializationLevel p_level);
void uninitialize_discord_module(godot::ModuleInitializationLevel p_level);

// engineから呼ばれるGDExtension初期化入口。
extern "C" GDExtensionBool GDE_EXPORT discord_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization);
