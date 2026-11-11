#pragma once

#include "../JuceHeader.h"
#include "AppLogger.h"
#include "PlatformPaths.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

// Batch 3.2: always-on diagnostics for the piano-roll view state.
//
// The regular LOG()/AppLogger output is compiled out of Release builds
// (PITCHNET_LOGGING_ENABLED defaults to JUCE_DEBUG), which is exactly
// where the "all segments disappear" reports come from, so - mirroring
// the BigVGAN session log - this writer appends to its own file in the
// plugin logs folder in every build:
//   viewstate_<session>.txt
// Each line carries both view-state stores (the PianoRollComponent
// viewState values that viewport capture reads, and the CoordinateMapper
// values the renderers actually draw with) plus a summary of the bound
// project, so a single trial log can separate "the view state was
// corrupt/diverged" (C1) from "the project itself was replaced by an
// empty one" (C2).
namespace ViewStateDiagnostics
{
    // Bit-level finiteness checks. pitchnet_core / pitchnet_ui are
    // compiled with -ffast-math in Release, whose -ffinite-math-only
    // folds std::isfinite / std::isnan to constant results - useless
    // for guarding against exactly the NaN values at issue here.
    // Inspecting the exponent bits is integer arithmetic and is not
    // affected by fast-math.
    inline bool valueIsFinite(double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL;
    }

    inline bool valueIsFinite(float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7F800000U) != 0x7F800000U;
    }

    inline juce::String formatValue(double value)
    {
        if (!valueIsFinite(value))
        {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            if ((bits & 0x000FFFFFFFFFFFFFULL) != 0)
                return "NaN";
            return (bits & 0x8000000000000000ULL) != 0 ? "-Inf" : "+Inf";
        }
        return juce::String(value, 3);
    }

    inline juce::String formatViewState(const char *label,
                                        double scrollX, double scrollY,
                                        double pixelsPerSecond,
                                        double pixelsPerSemitone)
    {
        return juce::String(label) + "{scrollX=" + formatValue(scrollX) +
               ", scrollY=" + formatValue(scrollY) +
               ", pps=" + formatValue(pixelsPerSecond) +
               ", ppst=" + formatValue(pixelsPerSemitone) + "}";
    }

    inline juce::String formatProject(const char *label, int noteCount,
                                      int f0Frames, int waveformSamples)
    {
        return juce::String(label) + "{notes=" + juce::String(noteCount) +
               ", f0Frames=" + juce::String(f0Frames) +
               ", waveformSamples=" + juce::String(waveformSamples) + "}";
    }

    namespace detail
    {
        inline void appendLine(const juce::String &message)
        {
            auto logFile = PlatformPaths::getLogFile(
                "viewstate_" + AppLogger::getSessionId() + ".txt");
            if (logFile == juce::File())
                return;
            const auto timestamp =
                juce::Time::getCurrentTime().toString(true, true, true, true);
            logFile.appendText("[" + timestamp + "] " + message + "\n");
        }
    } // namespace detail

    inline void logLine(const juce::String &message)
    {
        // Batch 4 fix (0.4.2): head each session's log with the app
        // version, so a trial log names the build that produced it.
        // The session id is one per process, so once per process is
        // once per session file.
        static std::once_flag sessionHeaderFlag;
        std::call_once(sessionHeaderFlag, [] {
            detail::appendLine("session-start version=" +
                               AppLogger::getAppVersion());
        });
        detail::appendLine(message);
    }
} // namespace ViewStateDiagnostics
