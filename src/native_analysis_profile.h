#pragma once

namespace ldcompress {

enum class NativeAnalysisProfile {
    Exact,
    OrderGuessExactRice,
    OrderGuessMeanRice,
    SubdivideTukey3MeanRice,
};

inline const char* native_analysis_profile_name(NativeAnalysisProfile profile)
{
    switch (profile) {
    case NativeAnalysisProfile::Exact:
        return "exact";
    case NativeAnalysisProfile::OrderGuessExactRice:
        return "order-guess-exact-rice";
    case NativeAnalysisProfile::OrderGuessMeanRice:
        return "order-guess-mean-rice";
    case NativeAnalysisProfile::SubdivideTukey3MeanRice:
        return "subdivide-tukey3-mean-rice";
    }
    return "unknown";
}

}  // namespace ldcompress
