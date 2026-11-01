#include <universal/q_shared.h>
#include "ui_shared.h"
#include "ui_font_selection.h"
#include <client/client.h>

Font_s *UI_SelectFont(const ScreenPlacement *scrPlace,int fontEnum,float scale,const CachedAssets_t &assets,const dvar_s *smallDvar,const dvar_s *bigDvar,const dvar_s *extraBigDvar)
{
    float scalea; // [esp+10h] [ebp+10h]

    switch (fontEnum)
    {
    case 2:
        return assets.bigFont;
    case 3:
        return assets.smallFont;
    case 5:
        return assets.consoleFont;
    case 6:
        return assets.objectiveFont;
    }
    scalea = scrPlace->scaleVirtualToReal[1] * scale;
    if (fontEnum == 4)
    {
        if (smallDvar->current.value < (double)scalea)
        {
            if (bigDvar->current.value > (double)scalea)
                return assets.textFont;
            else
                return assets.boldFont;
        }
        else
        {
            return assets.smallFont;
        }
    }
    else if (smallDvar->current.value < (double)scalea)
    {
        if (extraBigDvar->current.value > (double)scalea)
        {
            if (bigDvar->current.value > (double)scalea)
                return assets.textFont;
            else
                return assets.bigFont;
        }
        else
        {
            return assets.extraBigFont;
        }
    }
    else
    {
        return assets.smallFont;
    }
}
