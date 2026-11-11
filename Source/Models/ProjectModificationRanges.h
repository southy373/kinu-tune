#pragma once

#include "../JuceHeader.h"
#include "../Utils/Constants.h"
#include "Project.h"

#include <algorithm>
#include <cmath>
#include <vector>

// Sample ranges within an ARA audio modification.
//
// An ARA project is stored in MODIFICATION time: sample zero is the start of
// the audio modification, not the start of the playback region and not the
// start of the host timeline. A modification may be referenced by several
// playback regions at different timeline positions, so region-relative and
// timeline-relative sample indices are both ambiguous here - only modification
// samples identify a position unambiguously.
//
// Everything below therefore speaks modification samples, and the two
// functions must agree on that or edits land in the wrong place.

using SampleRange = juce::Range<int>;

// Batch 5: the ARA analysis window. Analysis no longer reads the whole
// modification: it reads the union of every playback region referencing
// the modification, widened by this margin on both sides and clamped to
// the source. The margin is an internal constant, not a user setting -
// it exists so the pitch/segmentation context at a clip edge (and the
// ~0.7s of render context an edit at the edge needs) sits comfortably
// inside the analysed range, and so small host-side trims keep working
// without a re-analysis. Five seconds is roughly seven times the render
// context and far beyond the few hundred milliseconds where detection
// near a hard cut is unreliable.
inline constexpr double kAnalysisWindowMarginSeconds = 5.0;

// The window to read and analyse, in modification samples, for a region
// union of [unionStart, unionEnd) within a source of sourceSampleCount
// samples at sourceSampleRate. An empty/invalid union yields an empty
// range at the source start; the caller treats that as "no window" and
// falls back to the whole source.
inline juce::Range<juce::int64>
computeAnalysisWindowSamples(juce::int64 unionStart, juce::int64 unionEnd,
                             juce::int64 sourceSampleCount,
                             double sourceSampleRate) {
  if (sourceSampleCount <= 0 || sourceSampleRate <= 0.0 ||
      unionEnd <= unionStart)
    return {};

  const auto margin = static_cast<juce::int64>(
      std::llround(kAnalysisWindowMarginSeconds * sourceSampleRate));
  const juce::int64 start = std::max<juce::int64>(0, unionStart - margin);
  const juce::int64 end =
      std::min(sourceSampleCount, unionEnd + margin);
  if (end <= start)
    return {};
  return {start, end};
}

// Frames dirtied by editing, converted to modification samples and merged into
// disjoint ranges.
//
// Batch 5 note: projects are windowed (see computeAnalysisWindowSamples
// above), so these come out in WINDOW samples - the project's own sample
// grid, whose zero is the window start. That is deliberate: the sole
// production consumer feeds them to preserveProcessedAudioOutsideRanges
// as indices into the replacement buffer (also window samples), and the
// window origin enters that function's mapping through its start-offset
// arguments, not through these ranges.
//
// This previously subtracted the playback region's timeline start, producing
// region-relative samples, while its consumer below indexes a buffer that
// spans the whole modification. The ranges were displaced by the region's
// offset, so a freshly synthesised edit was preserved where nothing had
// changed and the actually edited samples were overwritten with stale audio.
// A fallback ("if the region-relative time is negative, use the absolute
// time") hid this for edits before the region start, which is why only the
// right-hand slice of a split ever misbehaved.
inline std::vector<SampleRange>
collectDirtyModificationSampleRanges(const Project &project,
                                     double sampleRate) {
  std::vector<SampleRange> ranges;
  if (sampleRate <= 0.0)
    return ranges;

  const auto toModificationSample = [](int frame) {
    return static_cast<int>(std::max<juce::int64>(
        0,
        static_cast<juce::int64>(frame) * static_cast<juce::int64>(HOP_SIZE)));
  };

  for (const auto &note : project.getNotes()) {
    if (!note.isDirty())
      continue;
    const int start = toModificationSample(note.getStartFrame());
    const int end = toModificationSample(note.getEndFrame());
    if (end > start)
      ranges.emplace_back(start, end);
  }

  if (project.hasF0DirtyRange()) {
    const auto [startFrame, endFrame] = project.getF0DirtyRange();
    const int start = toModificationSample(startFrame);
    const int end = toModificationSample(endFrame);
    if (end > start)
      ranges.emplace_back(start, end);
  }

  std::sort(ranges.begin(), ranges.end(),
            [](const auto &a, const auto &b) {
              return a.getStart() < b.getStart();
            });

  std::vector<SampleRange> merged;
  for (const auto &range : ranges) {
    if (merged.empty() || range.getStart() > merged.back().getEnd()) {
      merged.push_back(range);
      continue;
    }
    merged.back() = merged.back().getUnionWith(range);
  }

  return merged;
}

// Carry unchanged audio forward from the previously published render, so a
// partial resynthesis does not discard edits outside the ranges it rebuilt.
//
// changedRanges is indexed in the REPLACEMENT buffer's own samples; both
// buffers are located in modification time by their start offsets, which is
// what lets the two be at different rates or offsets.
inline void preserveProcessedAudioOutsideRanges(
    juce::AudioBuffer<float> &replacement, double replacementRate,
    juce::int64 replacementStartInModification,
    const juce::AudioBuffer<float> &previous, double previousRate,
    juce::int64 previousStartInModification,
    const std::vector<SampleRange> &changedRanges) {
  if (replacement.getNumSamples() <= 0 || previous.getNumSamples() <= 0 ||
      replacementRate <= 0.0 || previousRate <= 0.0 || changedRanges.empty())
    return;

  const int channels =
      std::min(replacement.getNumChannels(), previous.getNumChannels());
  if (channels <= 0)
    return;

  const auto isChanged = [&changedRanges](int replacementSample) {
    for (const auto &range : changedRanges)
      if (range.contains(replacementSample))
        return true;
    return false;
  };

  const double rateRatio = previousRate / replacementRate;
  for (int dst = 0; dst < replacement.getNumSamples(); ++dst) {
    if (isChanged(dst))
      continue;

    const auto modificationSample =
        replacementStartInModification + static_cast<juce::int64>(dst);
    const double previousSamplePosition =
        static_cast<double>(modificationSample - previousStartInModification) *
        rateRatio;
    const int previousSample =
        static_cast<int>(std::llround(previousSamplePosition));
    if (previousSample < 0 || previousSample >= previous.getNumSamples())
      continue;

    for (int ch = 0; ch < channels; ++ch)
      replacement.setSample(ch, dst, previous.getSample(ch, previousSample));
  }
}
