#include "daisysp.h"
#include "daisysp-lgpl.h"
#include "daisy_patch_sm.h"

#include "library/effects/LinkwitzRileyCrossover.h"
#include "library/effects/LinkwitzRileyCrossover.cpp"
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

#define SAMPLE_RATE 48000
#define REVERB_BUFFER_SIZE 48000
DSY_SDRAM_BSS float reverb_buffer[REVERB_BUFFER_SIZE];

#define PHASER_BUFFER_SIZE 4096
DSY_SDRAM_BSS float phaser_buffer[PHASER_BUFFER_SIZE];

static DaisyPatchSM hw;
static UIManager ui;

Svf 							svf_l, svf_r;
sudwalfulkaan::Saturation  		sat_l, sat_r;
sudwalfulkaan::SwfBiquad		shelf_l, shelf_r;
sudwalfulkaan::StereoReverbSc 	reverb;
sudwalfulkaan::StereoPhaser 	phaser;


bool	bypass	= true;
float 	smoothedReverb, smoothedShelf, smoothedGain, smoothedCutoff, effectFeedback;

enum EffectType {
	EFFECT_BYPASS,
    EFFECT_REVERB,
    EFFECT_PHASER,
    EFFECT_COUNT
};

EffectType currentEffect = EFFECT_BYPASS;

// Callback functions
void OnPotChanged(int index, float value) {
	switch(index) {
		case patch_sm::CV_1:
		case patch_sm::CV_4:
			hw.PrintLine("DEBUG: CV1 for effect feedback = %.2f", ui.GetPotValue(index));
			hw.PrintLine("DEBUG: CV4 for effect amount = %.2f", ui.GetPotValue(index));

			effectFeedback = ui.GetPotValue(patch_sm::CV_1)+ui.GetRawPotValue(patch_sm::CV_4);
			hw.PrintLine("DEBUG: Effect feedback = %.2f", effectFeedback);

			reverb.SetFeedback(effectFeedback);
			phaser.SetMix(effectFeedback);

			svf_l.SetFreq(ui.GetPotValue(patch_sm::CV_4));
			svf_r.SetFreq(ui.GetPotValue(patch_sm::CV_4));
			break;

		case patch_sm::CV_2:
			hw.PrintLine("DEBUG: CV2 for high shelf = %.2f", ui.GetPotValue(index));
			shelf_l.SetGainDb(value);
			shelf_r.SetGainDb(value);
			break;

		case patch_sm::CV_3:
			hw.PrintLine("DEBUG: CV3 for saturation = %.2f", ui.GetPotValue(index));
			sat_l.SetGain(value);
			sat_r.SetGain(value);
			break;			
	}
}

void OnToggleChanged(bool toggleState) {
	hw.PrintLine("Toggle is now %s\n", toggleState ? "ON" : "OFF");

}

void OnButtonPressed() {
	// Advance to next effect, wrap back to 0 when needed
	hw.PrintLine("Switched to effect: %d\n", currentEffect);
	currentEffect = static_cast<EffectType>((currentEffect + 1) % EFFECT_COUNT);
}

void UpdateValues() {	
	// Update pots
	for(int i=0; i<4; i++) {
		ui.SetPotValue(i, hw.GetAdcValue(i));
	}
	ui.Update();
	
	ui.SetPotChangedCallback(OnPotChanged);
	ui.SetToggleChangedCallback(OnToggleChanged);
	ui.SetButtonPressedCallback(OnButtonPressed);
}

void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size)
{
	float wetL, wetR;
	hw.ProcessAllControls();
	for (size_t i = 0; i < size; i++)
	{				
		float l = in[0][i];
		float r = in[1][i];

		// Add high
		l = shelf_l.Process(l);
		r = shelf_r.Process(r);

		// Saturate
		l = sat_l.Process(l);
		r = sat_r.Process(r);

		// Apply effect
		switch (currentEffect) {
			case EFFECT_REVERB:
				reverb.Process(l, r, &wetL, &wetR);
			
				l = l + wetL;
				r = r + wetR;	
				break;

			case EFFECT_PHASER:
				l = phaser.ProcessLeft(l);
				r = phaser.ProcessRight(r);
				break;

            case EFFECT_BYPASS:
            default:
            	break;
        }

		svf_l.Process(l);
		svf_r.Process(r);

		l = svf_l.High();
		r = svf_r.High();
		
		out[0][i] = fclamp(l, -1.0f, 1.0f);
		out[1][i] = fclamp(r, -1.0f, 1.0f);
	}
}

int main(void)
{
	hw.Init();
	hw.SetAudioBlockSize(4); // number of samples handled per callback
	hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
 
    hw.StartLog(true);
    hw.PrintLine("Súdwâlfulkaan birth...");   
	hw.StartAdc(); // Start the ADC conversions
	
	svf_l.Init(SAMPLE_RATE);
	svf_r.Init(SAMPLE_RATE);

	svf_l.SetRes(0.4f);
	svf_r.SetRes(0.4f);
	
	sat_l.Init(1,1,Saturation::ARCTAN_SATURATION);
	sat_r.Init(1,1,Saturation::ARCTAN_SATURATION);

	shelf_l.Init(SAMPLE_RATE);
	shelf_l.SetFreq(5000.f);           // High shelf start
	shelf_l.SetQ(0.707f);              // Q controls transition sharpness
	shelf_l.SetGainDb(0.f);            // Boost by 0 dB
	shelf_l.SetType(SwfBiquad::Type::HIGHSHELF);

	shelf_r.Init(SAMPLE_RATE);
	shelf_r.SetFreq(5000.f);           // High shelf start
	shelf_r.SetQ(0.707f);              // Q controls transition sharpness
	shelf_r.SetGainDb(0.f);            // Boost by 0 dB
	shelf_r.SetType(SwfBiquad::Type::HIGHSHELF);

	// Initialize reverb, feedback can be near infinitely
    reverb.Init(SAMPLE_RATE, reverb_buffer, REVERB_BUFFER_SIZE);
    reverb.SetFeedback(0.f);
    reverb.SetLowpassFreq(18000.0f);
	
	phaser.Init(SAMPLE_RATE);
    phaser.SetDelays(phaser_buffer, PHASER_BUFFER_SIZE);
    phaser.SetFreq(0.4f);
    phaser.SetFeedback(0.6f);
    phaser.SetDepth(0.85f);
    phaser.SetMix(0.5f);

	// Setup pot meters
	PotConfig configs[4] = {
		{0.f, 0.05f, 0.f, 1.f, MappingType::Linear},     // CV_1 linear 0..1
		{0.f, 0.1f, 0.f, 10.f, MappingType::Linear},     // CV_2 linear 0..10
		{0.f, 0.1f, 1.f, 10.f, MappingType::Linear},     // CV_3 linear 1..10
		{0.f, 0.1f, 20.f, 20000.f, MappingType::Log}     // CV_4 logarithmic 20..20k
	};
	
	ui.Init(configs);

	hw.StartAudio(AudioCallback);

	while(1) {
		UpdateValues();
		System::Delay(10);
	}
}
