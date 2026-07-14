#include "decodemanager.h"

#include "qdltpluginmanager.h"

DecodeManager &DecodeManager::instance()
{
    static DecodeManager manager;
    return manager;
}

void DecodeManager::decode(QDltPluginManager *pluginManager, QDltMsg &msg, bool enabled, bool silentMode)
{
    if(!enabled || pluginManager == nullptr)
    {
        return;
    }

    pluginManager->decodeMsg(msg, silentMode);
}
