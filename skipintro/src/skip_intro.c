// skip intro: bk boots into getDefaultBootMap() (src/core1/code_0.c), which is
// the rare logo map. from there the game plays the attract cutscenes until you
// press start and reach the file select. the game already has a boot path
// straight to the file select (getSpecialBootMap, used without the debug
// flag), so this just makes the default boot map the same.
//
// a patch (not a hook) because it has to change the return value.

#include "modding.h"
#include "PR/ultratypes.h"

#define MAP_91_FILE_SELECT 0x91

RECOMP_PATCH s32 getDefaultBootMap(void) {
    return MAP_91_FILE_SELECT;
}
