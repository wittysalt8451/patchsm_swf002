#include "StereoReverbSc.h"

using namespace daisysp;
using namespace sudwalfulkaan;

void StereoReverbSc::Init(float sample_rate, float* buffer, size_t size) {
    reverb_.Init(sample_rate);
}

void StereoReverbSc::SetFeedback(float feedback) {
    reverb_.SetFeedback(feedback);
}

void StereoReverbSc::SetLowpassFreq(float freq) {
    reverb_.SetLpFreq(freq);
}

int StereoReverbSc::Process(const float& inL, const float& inR, float* wetL, float* wetR) {
    return reverb_.Process(inL, inR, wetL, wetR);
}
