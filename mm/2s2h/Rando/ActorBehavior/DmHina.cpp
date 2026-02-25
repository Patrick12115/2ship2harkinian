#include "ActorBehavior.h"
#include "2s2h/Network/Archipelago/Archipelago.h"

void Rando::ActorBehavior::InitDmHinaBehavior() {
    COND_VB_SHOULD(VB_DRAW_BOSS_REMAINS, (IS_RANDO || IS_ARCHI), {
        Actor* actor = va_arg(args, Actor*);
        RandoCheckId checkId;
        switch (actor->params) {
            case 0: // Odolwa's Remains
                checkId = RC_WOODFALL_TEMPLE_BOSS_WARP;
                break;
            case 1: // Goht's Remains
                checkId = RC_SNOWHEAD_TEMPLE_BOSS_WARP;
                break;
            case 2: // Gyorg's Remains
                checkId = RC_GREAT_BAY_TEMPLE_BOSS_WARP;
                break;
            case 3: // Twinmold's Remains
                checkId = RC_STONE_TOWER_TEMPLE_INVERTED_BOSS_WARP;
                break;
            default:
                return;
        }
        *should = false;

        auto randoSaveCheck = RANDO_SAVE_CHECKS[checkId];
        // Do not display if already obtained (i.e. for repeat visits)
        if (!randoSaveCheck.obtained) {
            RandoItemId randoItemId = Rando::ConvertItem(randoSaveCheck.randoItemId, checkId);
            if (randoItemId == RI_JUNK) {
                randoItemId = Rando::CurrentJunkItem(checkId);
            }
            Rando::DrawItem(randoItemId, checkId, actor);
        }
    });
}
