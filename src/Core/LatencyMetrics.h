#pragma once
#include "AppRuntime.h"
#include <algorithm>

namespace RegionLens::native
{
    inline uint64_t QpcFrequency() noexcept
    { static const uint64_t value=[] {LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return uint64_t(f.QuadPart);}();return value; }
    inline uint64_t QpcNow() noexcept {LARGE_INTEGER t{};QueryPerformanceCounter(&t);return uint64_t(t.QuadPart);}
    inline uint64_t QpcMicros(uint64_t ticks) noexcept
    {return uint64_t(static_cast<long double>(ticks)*1000000/QpcFrequency());}
    // One instance per producer/thread, no allocation. Percentiles describe the
    // most recent bounded samples; count/max cover the whole report interval.
    class LatencySamples
    {
    public:
        void Add(uint64_t microseconds) noexcept
        {m_values[m_count++%m_values.size()]=microseconds;m_max=(std::max)(m_max,microseconds);}
        void Report(int stage,uint64_t lens=0,int64_t extra=0) noexcept
        {
            if (!m_count) return;
            auto sorted=m_values;auto size=(std::min)(m_count,uint64_t(sorted.size()));
            std::sort(sorted.begin(),sorted.begin()+size);
            auto percentile=[&](uint64_t p){return int64_t(sorted[(size-1)*p/100]);};
            Record(DiagnosticEvent::Latency,{stage,int64_t(m_count),percentile(50),percentile(95),percentile(99),
                int64_t(m_max),int64_t(size),extra},false,0,lens);
            m_count=m_max=0;
        }
    private:
        std::array<uint64_t,128> m_values{};
        uint64_t m_count{},m_max{};
    };
}
