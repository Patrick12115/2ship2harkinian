#include "ActorBehavior.h"
#include "2s2h/Network/Archipelago/Archipelago.h"

void Rando::ActorBehavior::InitEnMa4Behavior() {
    COND_VB_SHOULD(VB_ROMANI_CONSIDER_EPONA_SONG_GIVEN, (IS_RANDO || IS_ARCHI),
                   { *should = RANDO_SAVE_CHECKS[RC_ROMANI_RANCH_EPONAS_SONG].obtained; });

    COND_VB_SHOULD(VB_GIVE_ITEM_FROM_ROMANI, (IS_RANDO || IS_ARCHI), {
        *should = false;
        RANDO_SAVE_CHECKS[RC_ROMANI_RANCH_EPONAS_SONG].eligible = true;
    });
}
