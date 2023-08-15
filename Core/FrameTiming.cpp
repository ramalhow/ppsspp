// Frame timing
//
// A frame on the main thread should look a bit like this:
//
// 1. -- Wait for the right time to start the frame  (alternatively, see this is step 8).
// 2. Sample inputs (on some platforms, this is done continouously during step 3)
// 3. Run CPU
// 4. Submit GPU commands (there's no reason to ever wait before this).
// 5. -- Wait for the right time to present
// 6. Send Present command
// 7. Do other end-of-frame stuff
//
// To minimize latency, we should *maximize* 1 and *minimize* 5 (while still keeping some margin to soak up hitches).
// Additionally, if too many completed frames have been buffered up, we need a feedback mechanism, so we can temporarily
// artificially increase 1 in order to "catch the CPU up".
//
// There are some other things that can influence the frame timing:
// * Unthrottling. If vsync is off or the backend can change present mode dynamically, we can simply disable all waits during unthrottle.
// * Frame skipping. This gets complicated.
// * The game not actually asking for flips, like in static loading screens

#include "Common/Profiler/Profiler.h"
#include "Common/Log.h"
#include "Common/TimeUtil.h"

#include "Core/RetroAchievements.h"
#include "Core/CoreParameter.h"
#include "Core/Core.h"
#include "Core/Config.h"
#include "Core/HW/Display.h"
#include "Core/FrameTiming.h"

FrameTiming g_frameTiming;

inline Draw::PresentMode GetBestImmediateMode(Draw::PresentMode supportedModes) {
	if (supportedModes & Draw::PresentMode::MAILBOX) {
		return Draw::PresentMode::MAILBOX;
	} else {
		return Draw::PresentMode::IMMEDIATE;
	}
}

void FrameTiming::Reset(Draw::DrawContext *draw) {
	if (g_Config.bVSync || !(draw->GetDeviceCaps().presentModesSupported & (Draw::PresentMode::MAILBOX | Draw::PresentMode::IMMEDIATE))) {
		presentMode = Draw::PresentMode::FIFO;
		presentInterval = 1;
	} else {
		presentMode = GetBestImmediateMode(draw->GetDeviceCaps().presentModesSupported);
		presentInterval = 0;
	}
	setTimestepCalled_ = false;
}

Draw::PresentMode ComputePresentMode(Draw::DrawContext *draw, int *interval) {
	Draw::PresentMode mode = Draw::PresentMode::FIFO;

	if (draw->GetDeviceCaps().presentModesSupported & (Draw::PresentMode::IMMEDIATE | Draw::PresentMode::MAILBOX)) {
		// Switch to immediate if desired and possible.
		bool wantInstant = false;
		if (!g_Config.bVSync) {
			wantInstant = true;
		}

		if (PSP_CoreParameter().fastForward) {
			wantInstant = true;
		}
		if (PSP_CoreParameter().fpsLimit != FPSLimit::NORMAL) {
			int limit;
			if (PSP_CoreParameter().fpsLimit == FPSLimit::CUSTOM1)
				limit = g_Config.iFpsLimit1;
			else if (PSP_CoreParameter().fpsLimit == FPSLimit::CUSTOM2)
				limit = g_Config.iFpsLimit2;
			else
				limit = PSP_CoreParameter().analogFpsLimit;

			// For an alternative speed that is a clean factor of 60, the user probably still wants vsync.
			// TODO: Should take the user's display refresh rate into account...
			if (limit == 0 || (limit >= 0 && limit != 15 && limit != 30 && limit != 60)) {
				wantInstant = true;
			}
		}

		if (wantInstant && g_Config.bVSync && !draw->GetDeviceCaps().presentInstantModeChange) {
			// If in vsync mode (which will be FIFO), and the backend can't switch immediately,
			// stick to FIFO.
			wantInstant = false;
		}

		// The outer if checks that instant modes are available.
		if (wantInstant) {
			mode = GetBestImmediateMode(draw->GetDeviceCaps().presentModesSupported);
		}
	}

	*interval = (mode == Draw::PresentMode::FIFO) ? 1 : 0;
	return mode;
}

void FrameTiming::BeforeCPUSlice(const FrameHistoryBuffer &frameHistory) {
	cpuSliceStartTime = time_now_d();

	// Here we can examine the frame history for anomalies to correct.
	nudge_ = 0.0;

	const FrameTimeData &oldData = frameHistory[3];
	if (oldData.queuePresent == 0.0) {
		// No data to look at.
		return;
	}

	if (oldData.afterFenceWait - oldData.frameBegin > 0.001) {
		nudge_ = (oldData.afterFenceWait - oldData.frameBegin) * 0.1;
	}

	if (oldData.firstSubmit - oldData.afterFenceWait > cpuTime) {
		// Not sure how this grows so large sometimes.
		nudge_ = (oldData.firstSubmit - oldData.afterFenceWait - cpuTime) * 0.1;
	}
}

void FrameTiming::SetTimeStep(float scaledTimeStep) {
	_dbg_assert_(usePresentTiming);

	double now = time_now_d();

	cpuTime = now - cpuSliceStartTime;
	this->timeStep = scaledTimeStep;

	// Sync up lastPresentTime with the current time if it's way off. TODO: This should probably drift.
	if (lastPresentTime < now - 0.5f) {
		lastPresentTime = now;
	}
	setTimestepCalled_ = true;
}

void FrameTiming::AfterCPUSlice() {
	if (!setTimestepCalled_) {
		// We're in the menu or something.
		usePresentTiming = true;
		SetTimeStep(1.0f / 60.0f);
	}
}

void FrameTiming::BeforePresent() {
	if (!usePresentTiming)
		return;

	// Wait until we hit the next present time. Ideally we'll be fairly close here due to the previous AfterPresent wait.
	nextPresentTime = lastPresentTime + this->timeStep + nudge_;
	while (true) {
		double remaining = nextPresentTime - time_now_d();
		if (remaining <= 0.0)
			break;
		sleep_s(remaining);
	}

	lastPresentTime = nextPresentTime;
}

void FrameTiming::AfterPresent() {
	// Sleep slightly less time than all of the available room, in case of a CPU spike.
	// This should be a tweakable.
	const double margin = 2.0 * 0.001;  // 4 ms
	postSleep = this->timeStep - margin - cpuTime;

	if (postSleep > 0.0) {
		sleep_s(postSleep);
	}

	if (!usePresentTiming)
		return;
}
