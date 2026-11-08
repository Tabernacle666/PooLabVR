#include "../../labs/labovr/DiagnosticStatistics.h"
#include <cassert>
#include <cmath>

int main(){
    using lab::DiagnosticStatistics;
    auto empty=DiagnosticStatistics({});assert(empty.median==0&&empty.spikes==0);
    auto even=DiagnosticStatistics({4,1,3,2});assert(even.median==2.5&&even.p90==4);
    std::vector<double> baseline(90,10.);
    auto normal=DiagnosticStatistics(baseline);
    baseline.back()=525.;
    auto stalled=DiagnosticStatistics(baseline);
    assert(normal.median==stalled.median&&normal.trimmedMean==stalled.trimmedMean);
    assert(stalled.p90==10.&&stalled.spikes==1);
    // A sustained slowdown must remain visible; trimming cannot erase it.
    std::fill(baseline.begin(),baseline.end(),25.);
    auto slow=DiagnosticStatistics(baseline);
    assert(slow.median==25.&&slow.trimmedMean==25.&&slow.spikes==0);
    std::fill(baseline.begin(),baseline.begin()+18,50.);
    auto recurring=DiagnosticStatistics(baseline);
    assert(recurring.p90==50.&&recurring.trimmedMean>25.);
    auto single=DiagnosticStatistics({7.});assert(single.median==7.&&single.trimmedMean==7.);
}
