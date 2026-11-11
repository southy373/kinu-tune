#pragma once

#include "../JuceHeader.h"

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
// on any field that BOTH sides know refuses the payload (the live
// modification starts new). A field either side does not know (an
// unreadable source, a zero length) never refuses on its own.
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
    disagree. When `field` is given it receives the first mismatching
    field's name ("length" / "fingerprint" / "sourceId"). */
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
  if (stored.hasFingerprint && live.hasFingerprint &&
      std::abs(stored.fingerprint - live.fingerprint) > 1.0e-9)
    return differ("fingerprint");
  if (stored.sourceId.isNotEmpty() && live.sourceId.isNotEmpty() &&
      stored.sourceId != live.sourceId)
    return differ("sourceId");
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
