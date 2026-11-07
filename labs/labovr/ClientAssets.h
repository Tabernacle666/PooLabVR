#pragma once
#include "Assets.h"
namespace lab {
SceneData CreateClientAssetSet();
void AppendClientScene(SceneData&,const SceneData&);
}
