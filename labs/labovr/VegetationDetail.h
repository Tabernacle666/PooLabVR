#pragma once

namespace lab::urban {
enum class TreeDetail { Close, Middle, Distant };

// Distances are horizontal metres from the viewer to the tree's origin.
constexpr TreeDetail TreeDetailAt(double distanceSquared) noexcept {
    return distanceSquared<=30.0*30.0 ? TreeDetail::Close :
           distanceSquared<=80.0*80.0 ? TreeDetail::Middle : TreeDetail::Distant;
}
}
