#pragma once
#include "Understory.h"
#include <optional>

namespace OASIS {
// The extension borrows precisely the same immutable source meshes as
// the authored campus. No extra textures, recipes or botanical geometry.
struct ExpansionFlora {
    std::array<WorldObject,9> floor{};
    bool hasFloor{};
    std::array<WorldObject,Understory::ModelCount> understory{};
    std::array<bool,Understory::ModelCount> used{};
    std::optional<WorldObject> insects;
};
}
