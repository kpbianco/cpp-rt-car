#include "Modules/ModuleManager.h"

class FRTFWUnrealTestHostModule final : public IModuleInterface
{
};

IMPLEMENT_PRIMARY_GAME_MODULE(
    FRTFWUnrealTestHostModule,
    RTFWUnrealTestHost,
    "RTFWUnrealTestHost")
