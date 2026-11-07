#include "../labs/labovr/PopulationContinuityCheck.h"
#include <cassert>
#include <cmath>
#include <limits>

int main(){
    lab::PopulationContinuityCheck check;
    LabPopulationSnapshotV1 s{};
    s.voiceCount=1;s.voices[0].insect=42;s.voices[0].position={1,2,3};
    s.sourceFrame=100;
    assert(!check.Observe(s,100)); // Before/at hiding is not a valid baseline.
    s.sourceFrame=101;
    assert(!check.Observe(s,100)); // Baseline is actually hidden.
    s.sourceFrame=108;s.voices[0].position.x=2;
    assert(!check.Observe(s,100)); // At least 8 frame IDs must separate samples.
    s.sourceFrame=109;s.voices[0].insect=43;
    assert(!check.Observe(s,100)); // Different insect cannot prove movement.
    assert(check.Comparisons()==0);
    s.voices[0].insect=42;s.voices[0].position.x=1;
    assert(!check.Observe(s,100)); // Unchanged state is not success.
    assert(check.Comparisons()==1);
    s.voices[0].position.x=std::numeric_limits<float>::quiet_NaN();
    assert(!check.Observe(s,100)); // Invalid state cannot prove movement.
    assert(check.Comparisons()==1);
    s.voices[0].position.x=1.01f;
    assert(check.Observe(s,100));
    assert(check.Witness()==42 && check.Distance()>.009);
    assert(check.BaselineFrame()==101);
}
