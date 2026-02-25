#include "ActorBehavior.h"
#include "2s2h/Network/Archipelago/Archipelago.h"

void Rando::ActorBehavior::InitEnHgBehavior() {
    COND_VB_SHOULD(VB_HAVE_HEALED_PAMELAS_FATHER, (IS_RANDO || IS_ARCHI),
                   { *should = RANDO_SAVE_CHECKS[RC_MUSIC_BOX_HOUSE_FATHER].cycleObtained; });
}