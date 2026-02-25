#include "ActorBehavior.h"
#include "2s2h/Network/Archipelago/Archipelago.h"

void Rando::ActorBehavior::InitEnShnBehavior() {

    COND_VB_SHOULD(VB_EXEC_MSG_EVENT, (IS_RANDO || IS_ARCHI), {
        u32 cmdId = va_arg(args, u32);
        Actor* actor = va_arg(args, Actor*);

        if (actor->id == ACTOR_EN_SHN && cmdId == MSCRIPT_CMD_ID_OFFER_ITEM) { // Swamp Tourist Center Guide
            MsgScriptCmdOfferItem* cmd = va_arg(args, MsgScriptCmdOfferItem*);
            GetItemId getItemId = (GetItemId)SCRIPT_PACK_16(cmd->itemIdH, cmd->itemIdL);
            if (getItemId == GI_HEART_PIECE) { // Showed picture of Tingle or the Deku King
                // Do not do any substituted behavior, just skip this single command
                *should = false;
            }
        }
    });
}
