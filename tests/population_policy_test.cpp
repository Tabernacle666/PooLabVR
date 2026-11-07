#include "../src/core/PopulationPolicy.h"
#include <cassert>
#include <initializer_list>

int main() {
    // A hidden, still-loaded lab must continue evolving without a remote player.
    auto hidden=lab::PopulationPolicy(true,true,false);
    assert(hidden.simulate && !hidden.observerPresent);
    auto visible=lab::PopulationPolicy(true,true,true);
    assert(visible.simulate && visible.observerPresent);
    // Explicitly disabled populations and unallocated scenes must not step.
    for(bool observer: {false,true}) {
        assert(!lab::PopulationPolicy(true,false,observer).simulate);
        assert(!lab::PopulationPolicy(false,true,observer).simulate);
        assert(!lab::PopulationPolicy(false,false,observer).simulate);
    }
}
