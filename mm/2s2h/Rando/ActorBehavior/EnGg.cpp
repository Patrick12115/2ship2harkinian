#include "ActorBehavior.h"
#include "2s2h/Network/Archipelago/Archipelago.h"

// This accounts for both EnGg and EnGg2, both of which act as Darmani's ghost.
void Rando::ActorBehavior::InitEnGgBehavior() {
    COND_VB_SHOULD(VB_CONSIDER_DARMANI_HEALED, (IS_RANDO || IS_ARCHI),
                   { *should = RANDO_SAVE_CHECKS[RC_GORON_GRAVEYARD_DARMANI].obtained; });
}