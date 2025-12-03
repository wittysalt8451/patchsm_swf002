#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "daisy_patch_sm.h"

#include "library/effects/Saturation.h"
#include "library/effects/Saturation.cpp"
#include "library/ui/UIManager.h"
#include "library/ui/UIManager.cpp"
#include "library/effects/SwfBiquad.h"
#include "library/effects/SwfBiquad.cpp"
#include "library/effects/StereoReverbSc.h"
#include "library/effects/StereoReverbSc.cpp"
#include "library/effects/StereoPhaser.h"
#include "library/effects/StereoPhaser.cpp"
#include "library/tools.cpp"

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;
using namespace sudwalfulkaan;

// Constants
#define SAMPLE_RATE 48000
#define REVERB_BUFFER_SIZE 48000
#define PHASER_BUFFER_SIZE 4096
#define NUM_POTS 4
#define AUDIO_BLOCK_SIZE 4
#define SPLASH_DECAY_TIME_SEC 4.0f
#define SPLASH_THRESHOLD 0.1f
#define SPLASH_ATTACK_RATE 0.01f
#define SPLASH_VOLUME_SCALE 0.5f
#define SPLASH_ENV_MIN 0.0001f

// SDRAM buffers
DSY_SDRAM_BSS float reverb_buffer[REVERB_BUFFER_SIZE];
DSY_SDRAM_BSS float phaser_buffer[PHASER_BUFFER_SIZE];

// Pre-calculated constants
const float SPLASH_DECAY_COEFF = expf(-1.0f / (SAMPLE_RATE * SPLASH_DECAY_TIME_SEC));

// Hardware and UI
static DaisyPatchSM hw;
static UIManager ui;

// Audio processing objects
sudwalfulkaan::Saturation sat_l, sat_r;
sudwalfulkaan::SwfBiquad shelf_l, shelf_r;
sudwalfulkaan::SwfBiquad scoop_l, scoop_r;
sudwalfulkaan::StereoReverbSc reverb;
sudwalfulkaan::StereoPhaser phaser;
daisysp::WhiteNoise splashNoise;
daisysp::Tone splashTone;
daisysp::Svf svf_l, svf_r, splashFilter;

// State variables
bool splashToggle = false;
bool splashActive = false;
float splash = 0.0f;
float splashEnv = 0.0f;
float effectFeedback = 0.0f;

// Effect selection
enum EffectType {
    EFFECT_BYPASS,
    EFFECT_REVERB,
    EFFECT_PHASER,
    EFFECT_COUNT
};

EffectType currentEffect = EFFECT_BYPASS;

// Helper function to zero a buffer
void ZeroBuffer(float* buffer, size_t size) {
    for (size_t i = 0; i < size; i++) {
        buffer[i] = 0.0f;
    }
}

// Helper function to initialize a biquad filter
void InitBiquad(SwfBiquad& filter, float sampleRate, float freq, float q, float gainDb, SwfBiquad::Type type) {
    filter.Init(sampleRate);
    filter.SetFreq(freq);
    filter.SetQ(q);
    filter.SetGainDb(gainDb);
    filter.SetType(type);
}

// Helper function to update effect feedback
void UpdateEffectFeedback() {
    effectFeedback = ui.GetPotValue(patch_sm::CV_1) + ui.GetRawPotValue(patch_sm::CV_4);
    reverb.SetFeedback(effectFeedback);
    phaser.SetMix(effectFeedback);
}

// Callback functions
void OnPotChanged(int index, float value) {
    switch (index) {
        case patch_sm::CV_1:
            UpdateEffectFeedback();
            break;

        case patch_sm::CV_2:
            shelf_l.SetGainDb(value);
            shelf_r.SetGainDb(value);
            scoop_l.SetGainDb(value * 0.5f);
            scoop_r.SetGainDb(value * 0.5f);
            break;

        case patch_sm::CV_3:
            sat_l.SetGain(value);
            sat_r.SetGain(value);
            break;

        case patch_sm::CV_4: {
            UpdateEffectFeedback();
            
            float cutoffFreq = ui.GetPotValue(patch_sm::CV_4);
            svf_l.SetFreq(cutoffFreq);
            svf_r.SetFreq(cutoffFreq);

            // Splash logic: start when opening high pass, decay when closing
            float potValue = ui.GetRawPotValue(patch_sm::CV_4);
            
            if (potValue > SPLASH_THRESHOLD) {
                // Opening the high pass - gradually increase splash
                if (!splashActive) {
                    splashActive = true;
                    splashEnv = 0.0f;
                }
                // Gradually increase volume based on pot position, but don't decrease when turning down
                float targetVolume = potValue;
                if (targetVolume > splashEnv) {
                    splashEnv = fclamp(splashEnv + SPLASH_ATTACK_RATE, 0.0f, targetVolume);
                }
            }
            break;
        }
    }
}

void OnToggleChanged(bool toggleState) {
    hw.PrintLine("Noise splash is now %s\n", toggleState ? "ON" : "OFF");
    splashToggle = toggleState;
}

void OnButtonPressed() {
    currentEffect = static_cast<EffectType>((currentEffect + 1) % EFFECT_COUNT);
    hw.PrintLine("Switched to effect: %d\n", currentEffect);
}

void UpdateValues() {
    // Update pot values
    for (int i = 0; i < NUM_POTS; i++) {
        ui.SetPotValue(i, hw.GetAdcValue(i));
    }
    ui.Update();
    
    // Set callbacks only once
    static bool callbacksSet = false;
    if (!callbacksSet) {
        ui.SetPotChangedCallback(OnPotChanged);
        ui.SetToggleChangedCallback(OnToggleChanged);
        ui.SetButtonPressedCallback(OnButtonPressed);
        callbacksSet = true;
    }
}

void ProcessSplash(float& left, float& right) {
    // Always reset splash to prevent noise from stale values
    splash = 0.0f;
    
    if (splashToggle && splashActive) {
        // Check if we should start decaying
        if (ui.GetRawPotValue(patch_sm::CV_4) <= SPLASH_THRESHOLD) {
            splashEnv *= SPLASH_DECAY_COEFF;
            if (splashEnv < SPLASH_ENV_MIN) {
                splashActive = false;
                splashEnv = 0.0f;
            }
        }
        
        // Generate and filter splash noise
        splash = splashNoise.Process();
        splash = splashTone.Process(splash);
        splashFilter.Process(splash);
        splash = splashFilter.High();
        splash *= splashEnv;
    }
    
    // Mix splash into audio
    left += splash * SPLASH_VOLUME_SCALE;
    right += splash * SPLASH_VOLUME_SCALE;
}

void ProcessEffect(float& left, float& right) {
    float wetL, wetR;
    
    switch (currentEffect) {
        case EFFECT_REVERB:
            reverb.Process(left, right, &wetL, &wetR);
            left += wetL;
            right += wetR;
            break;

        case EFFECT_PHASER:
            left = phaser.ProcessLeft(left);
            right = phaser.ProcessRight(right);
            break;

        case EFFECT_BYPASS:
        default:
            break;
    }
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    hw.ProcessAllControls();
    
    for (size_t i = 0; i < size; i++) {
        float left = in[0][i];
        float right = in[1][i];

        // EQ processing
        left = shelf_l.Process(left);
        right = shelf_r.Process(right);
        left = scoop_l.Process(left);
        right = scoop_r.Process(right);
        
        // Saturation
        left = sat_l.Process(left);
        right = sat_r.Process(right);

        // Apply effect
        ProcessEffect(left, right);

        // Process splash noise
        ProcessSplash(left, right);

        // High-pass filter
        svf_l.Process(left);
        svf_r.Process(right);
        left = svf_l.High();
        right = svf_r.High();
        
        // Output with clamping
        out[0][i] = fclamp(left, -1.0f, 1.0f);
        out[1][i] = fclamp(right, -1.0f, 1.0f);
    }
}

void InitializeFilters() {
    // SVF filters
    svf_l.Init(SAMPLE_RATE);
    svf_r.Init(SAMPLE_RATE);
    svf_l.SetRes(0.4f);
    svf_r.SetRes(0.4f);
    
    // Saturation
    sat_l.Init(1, 1, Saturation::ARCTAN_SATURATION);
    sat_r.Init(1, 1, Saturation::ARCTAN_SATURATION);

    // High shelf filters (5kHz)
    InitBiquad(shelf_l, SAMPLE_RATE, 5000.0f, 0.707f, 0.0f, SwfBiquad::Type::HIGHSHELF);
    InitBiquad(shelf_r, SAMPLE_RATE, 5000.0f, 0.707f, 0.0f, SwfBiquad::Type::HIGHSHELF);

    // Scoop filters (800Hz)
    InitBiquad(scoop_l, SAMPLE_RATE, 800.0f, 1.0f, -10.0f, SwfBiquad::Type::PEAK);
    InitBiquad(scoop_r, SAMPLE_RATE, 800.0f, 1.0f, -10.0f, SwfBiquad::Type::PEAK);
}

void InitializeEffects() {
    // Initialize reverb buffer to zero to prevent noise
    ZeroBuffer(reverb_buffer, REVERB_BUFFER_SIZE);
    reverb.Init(SAMPLE_RATE, reverb_buffer, REVERB_BUFFER_SIZE);
    reverb.SetFeedback(0.0f);
    reverb.SetLowpassFreq(18000.0f);
    
    // Initialize phaser
    phaser.Init(SAMPLE_RATE);
    phaser.SetDelays(phaser_buffer, PHASER_BUFFER_SIZE);
    phaser.SetFreq(0.4f);
    phaser.SetFeedback(0.6f);
    phaser.SetDepth(0.85f);
    phaser.SetMix(0.5f);
}

void InitializeSplash() {
    splashNoise.Init();
    splashTone.Init(SAMPLE_RATE);
    splashTone.SetFreq(4000.0f);
    
    splashFilter.Init(SAMPLE_RATE);
    splashFilter.SetFreq(2000.0f);
    splashFilter.SetRes(0.7f);
}

int main(void) {
    // Hardware initialization
    hw.Init();
    hw.SetAudioBlockSize(AUDIO_BLOCK_SIZE);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.PrintLine("Súdwâlfulkaan birth...");
    hw.StartAdc();
    
    // Initialize audio processing
    InitializeFilters();
    InitializeEffects();
    InitializeSplash();

    // Setup UI
    PotConfig configs[NUM_POTS] = {
        {0.0f, 0.05f, 0.0f, 1.0f, MappingType::Linear},      // CV_1: Effect feedback (0..1)
        {0.0f, 0.1f, 0.0f, 10.0f, MappingType::Linear},       // CV_2: EQ gain (0..10)
        {0.0f, 0.1f, 1.0f, 10.0f, MappingType::Linear},       // CV_3: Saturation (1..10)
        {0.0f, 0.1f, 20.0f, 20000.0f, MappingType::Log}      // CV_4: High-pass frequency (20..20k Hz)
    };
    ui.Init(configs);

    // Wait for controls to stabilize
    System::Delay(100);

    // Start audio processing
    hw.StartAudio(AudioCallback);

    // Main loop
    while (1) {
        UpdateValues();
        System::Delay(10);
    }
}
