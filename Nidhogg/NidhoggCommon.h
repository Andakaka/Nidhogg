#pragma once
#include "pch.h"

// #define DRIVER_REFLECTIVELY_LOADED // Comment or uncomment it when you load the driver reflectively.
#define DRIVER_TAG 'hdiN'
#define DRIVER_PREFIX "Nidhogg: "
constexpr SIZE_T MAX_PATH = 260;

// Globals.
inline PVOID RegistrationHandle = NULL;

struct EnabledFeatures {
	bool DriverReflectivelyLoaded = false;
	bool FunctionPatching		  = true;
	bool ModuleHiding			  = true;
	bool WriteData				  = true;
	bool ReadData				  = true;
	bool RegistryFeatures		  = true;
	bool ProcessProtection		  = true;
	bool ThreadProtection		  = true;
	bool FileProtection			  = true;
	bool EtwTiTamper			  = true;
	bool ApcInjection			  = true;
	bool CreateThreadInjection	  = false;
};
inline EnabledFeatures Features;
