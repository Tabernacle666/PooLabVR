#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lab {
struct DiagnosticDistribution {
    double median{},p90{},trimmedMean{},spikeThreshold{};
    uint32_t spikes{};
};
// Descriptive statistics, not an attribution of stalls to external software.
// Trim both tails by 10%; retain every raw sample and report upper outliers.
inline DiagnosticDistribution DiagnosticStatistics(std::vector<double> values) {
    DiagnosticDistribution result;
    if(values.empty())return result;
    std::sort(values.begin(),values.end());
    const size_t n=values.size();
    result.median=n%2?values[n/2]:(values[n/2-1]+values[n/2])*.5;
    result.p90=values[size_t(std::ceil(n*.9))-1];
    const size_t trim=n/10;
    for(size_t i=trim;i<n-trim;++i)result.trimmedMean+=values[i];
    result.trimmedMean/=double(n-2*trim);
    std::vector<double> deviations;deviations.reserve(n);
    for(double v:values)deviations.push_back(std::abs(v-result.median));
    std::sort(deviations.begin(),deviations.end());
    const double mad=n%2?deviations[n/2]:(deviations[n/2-1]+deviations[n/2])*.5;
    result.spikeThreshold=result.median+std::max(2.,6.*1.4826*mad);
    for(double v:values)result.spikes+=v>result.spikeThreshold?1u:0u;
    return result;
}
}
