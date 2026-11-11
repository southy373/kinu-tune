#pragma once

#include "../JuceHeader.h"

#include <algorithm>
#include <cmath>

// Batch 5.1 (item 11): source identity recorded in the ARA
// modification archive (format v5) and checked at restore time.
//
// Until v4 the archive recorded nothing about WHICH audio source a
// modification's payload belonged to, and restore trusted the host
// filter's persistent-ID match completely - so if a host ever
// re-presented one modification's payload for another (the
// Make-Permanent pool-resend case), the plug-in accepted edits and
// rendered audio that belonged to a different take. From v5 each
// modification entry also records the identity of its audio source:
// the source's persistent ID, its length in samples, and a cheap
// content fingerprint (fixed probe windows, computed by the document
// controller - the ARA-aware side). At restore, a definite mismatch
// on a content-derived field (length or fingerprint) that BOTH
// sides know refuses the payload (the live modification starts
// new). A field either side does not know (an unreadable source, a
// zero length) never refuses on its own. The persistent ID is
// recorded and printed in the diagnostics but never refuses on its
// own - see araSourceIdentityMismatch below.
//
// This deliberately cannot stop the same-FILE re-attachment: for the
// same file every field legitimately matches. That case is the
// destroyed-ID denylist's job (item 10, in the document controller).
struct AraSourceIdentity {
  juce::String sourceId;
  juce::int64 sampleCount = 0;
  double fingerprint = 0.0;
  bool hasFingerprint = false;
};

/** True when the stored identity and the live source definitely
    disagree on a content-derived field. When `field` is given it
    receives the first mismatching field's name ("length" /
    "fingerprint").

    The source persistent ID is NOT a refusal condition (batch 5.1
    review, B1): hosts may rewrite persistent IDs across saves -
    this codebase already designs around exactly that for
    modification IDs (the REAPER rewrite documented in
    PitchNetAudioModification.h, which is why live identity there
    is a process-local serial, not the persistent ID). An ID-only
    difference therefore cannot distinguish a hostile
    re-presentation from a legitimate restore of the same source
    after the host renumbered it, and refusing on it would silently
    discard the edits of a perfectly ordinary project. The ID stays
    recorded in the archive and printed in the diagnostics
    (restoreDenied lines carry both sides), where it remains useful
    evidence; the refusal rests on length and fingerprint, which
    derive from the source content and do not depend on host ID
    bookkeeping. */
inline bool araSourceIdentityMismatch(const AraSourceIdentity &stored,
                                      const AraSourceIdentity &live,
                                      juce::String *field = nullptr) {
  const auto differ = [field](const char *name) {
    if (field != nullptr)
      *field = name;
    return true;
  };
  if (stored.sampleCount > 0 && live.sampleCount > 0 &&
      stored.sampleCount != live.sampleCount)
    return differ("length");
  if (stored.hasFingerprint && live.hasFingerprint) {
    // Batch 5.1 review (M4): the fingerprint is a tap-weighted sum
    // of up to 1536 samples and reaches ~1e6..1e8 in magnitude,
    // where the old absolute 1.0e-9 threshold is at/below one
    // double ULP and demanded bit-exact reads across store and
    // restore. Compare relatively instead: 1e-6 of the larger
    // magnitude is orders of magnitude above floating-point
    // accumulation noise at any scale, and orders of magnitude
    // below the O(1) relative separation between genuinely
    // different takes, so real mismatches are still caught.
    const double scale = std::max(std::abs(stored.fingerprint),
                                  std::abs(live.fingerprint));
    if (std::abs(stored.fingerprint - live.fingerprint) > 1.0e-6 * scale)
      return differ("fingerprint");
  }
  return false;
}

/** v5 stream encoding of one identity record, written right after
    the modification's persistent ID. */
inline void writeAraSourceIdentity(juce::OutputStream &out,
                                   const AraSourceIdentity &identity) {
  out.writeString(identity.sourceId);
  out.writeInt64(identity.sampleCount);
  out.writeDouble(identity.fingerprint);
  out.writeInt(identity.hasFingerprint ? 1 : 0);
}

inline AraSourceIdentity readAraSourceIdentity(juce::InputStream &in) {
  AraSourceIdentity identity;
  identity.sourceId = in.readString();
  identity.sampleCount = in.readInt64();
  identity.fingerprint = in.readDouble();
  identity.hasFingerprint = in.readInt() != 0;
  return identity;
}

/** One-line rendering for ARA_DIAG output. */
inline juce::String describeAraSourceIdentity(
    const AraSourceIdentity &identity) {
  return "srcId=" +
         (identity.sourceId.isEmpty() ? juce::String("none")
                                      : identity.sourceId) +
         " len=" + juce::String(identity.sampleCount) +
         " fp=" +
         (identity.hasFingerprint
              ? juce::String(identity.fingerprint, 6)
              : juce::String("none"));
}
