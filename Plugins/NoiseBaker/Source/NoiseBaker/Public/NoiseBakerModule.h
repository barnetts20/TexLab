#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** Runtime module for the Noise Baker plugin.
 *
 *  Loads at PostConfigInit purely so it can register the virtual shader source
 *  directory "/Plugin/NoiseBaker" before the shader compiler starts walking
 *  global shader types. Registering later than PostConfigInit means the global
 *  shader map is built without our .usf and the first dispatch asserts. */
class FNoiseBakerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
