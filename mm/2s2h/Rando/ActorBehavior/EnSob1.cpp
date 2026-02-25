#include "ActorBehavior.h"
#include "2s2h/Network/Archipelago/Archipelago.h"

extern "C" {
#include "overlays/actors/ovl_En_Sob1/z_en_sob1.h"
}

void EnSob1_DrawCustomItem(Actor* thisx, PlayState* play) {
    auto randoSaveCheck = RANDO_SAVE_CHECKS[RC_BOMB_SHOP_ITEM_01];

    Matrix_Scale(20.0f, 20.0f, 20.0f, MTXMODE_APPLY);
    Matrix_Translate(23.0f, 0.0f, -43.0f, MTXMODE_APPLY);

    s16 rotX = (s16)((-90.0f / 180.0f) * 32768.0f);
    Matrix_RotateZYX(rotX, 0, 0, MTXMODE_APPLY);

    // Convert the item before drawing (e.g., bombs -> junk if no bomb bag, duplicate masks -> junk)
    RandoItemId randoItemId = Rando::ConvertItem(randoSaveCheck.randoItemId, RC_BOMB_SHOP_ITEM_01);
    if (randoItemId == RI_JUNK) {
        randoItemId = Rando::CurrentJunkItem(RC_BOMB_SHOP_ITEM_01);
    }
    Rando::DrawItem(randoItemId, RC_BOMB_SHOP_ITEM_01, thisx);
}

static MtxF sLeftHandMtxF;

void Rando::ActorBehavior::InitEnSob1Behavior() {
    COND_VB_SHOULD(VB_DRAW_ITEM_FROM_SOB1, (IS_RANDO || IS_ARCHI), {
        Actor* actor = va_arg(args, Actor*);
        if (RANDO_SAVE_CHECKS[RC_BOMB_SHOP_ITEM_01].shuffled) {
            Matrix_MtxFCopy(&sLeftHandMtxF, Matrix_GetCurrent());
            *should = false;
        }
    });

    COND_ID_HOOK(OnActorDraw, ACTOR_EN_OSSAN, (IS_RANDO || IS_ARCHI), [](Actor* actor) {
        if (RANDO_SAVE_CHECKS[RC_BOMB_SHOP_ITEM_01].shuffled && actor->params == 2) { // Bomb Shop Owner
            Matrix_Put(&sLeftHandMtxF);
            EnSob1_DrawCustomItem(actor, gPlayState);
        }
    });
}