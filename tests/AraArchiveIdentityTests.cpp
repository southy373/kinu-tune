// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

// Batch 5.1 (item 11): the ARA modification archive v5 source
// identity - the mismatch decision and the stream codec.
//
// The full store/restore stream functions need a bound ARA document
// and a host filter, which no unit test in this tree constructs; what
// is pinned here is everything those functions delegate to: which
// field combinations refuse a restore, and that the v5 identity
// record round-trips byte-exact. The v4 readability side is
// structural (restore reads identity fields only when version >= 5
// and the accepted version range is unchanged), and is covered by
// the legacy case below: an identity record that was never written
// is the default-constructed one, and it mismatches nothing.
#include "../Source/Plugin/AraArchiveIdentity.h"
#include "TestAssert.h"

#include <iostream>

namespace {

AraSourceIdentity fullIdentity() {
  AraSourceIdentity identity;
  identity.sourceId = "source-abc";
  identity.sampleCount = 441000;
  identity.fingerprint = -1234.5678;
  identity.hasFingerprint = true;
  return identity;
}

void testMatchingIdentitiesAreAccepted() {
  const auto stored = fullIdentity();
  juce::String field;
  CHECK(!araSourceIdentityMismatch(stored, fullIdentity(), &field));
  std::cout << "matching identities accepted: ok\n";
}

void testContentFieldMismatchRefuses() {
  const auto stored = fullIdentity();

  auto byLength = fullIdentity();
  byLength.sampleCount = 441001;
  juce::String field;
  CHECK(araSourceIdentityMismatch(stored, byLength, &field));
  CHECK(field == "length");

  auto byFingerprint = fullIdentity();
  byFingerprint.fingerprint = -1234.5;
  CHECK(araSourceIdentityMismatch(stored, byFingerprint, &field));
  CHECK(field == "fingerprint");

  std::cout << "content field mismatch refuses: ok\n";
}

void testSourceIdOnlyMismatchIsAccepted() {
  // Review B1: the persistent ID is recorded and logged, but an
  // ID-only difference never refuses - hosts may rewrite persistent
  // IDs across saves, so it cannot distinguish a hostile
  // re-presentation from a legitimate restore of the same source.
  const auto stored = fullIdentity();
  auto bySourceId = fullIdentity();
  bySourceId.sourceId = "source-xyz";
  juce::String field;
  CHECK(!araSourceIdentityMismatch(stored, bySourceId, &field));

  std::cout << "sourceId-only mismatch accepted: ok\n";
}

void testFingerprintRelativeTolerance() {
  // Review M4: the fingerprint comparison is relative (1e-6 of the
  // larger magnitude), not the old absolute 1e-9 - at fingerprint
  // magnitudes (~1e6..1e8) the absolute threshold sat at/below one
  // double ULP and demanded bit-exact reads.
  const auto withFingerprint = [](double fp) {
    AraSourceIdentity identity;
    identity.sampleCount = 441000;
    identity.fingerprint = fp;
    identity.hasFingerprint = true;
    return identity;
  };

  juce::String field;
  // Top of the range (~1e8): +50 is 5e-7 relative - within
  // tolerance, accepted.
  CHECK(!araSourceIdentityMismatch(withFingerprint(1.0e8),
                                   withFingerprint(1.0e8 + 50.0), &field));
  // +200 is 2e-6 relative - beyond tolerance, refused.
  CHECK(araSourceIdentityMismatch(withFingerprint(1.0e8),
                                  withFingerprint(1.0e8 + 200.0), &field));
  CHECK(field == "fingerprint");
  // Negative fingerprints compare by magnitude the same way.
  CHECK(!araSourceIdentityMismatch(withFingerprint(-1.0e8),
                                   withFingerprint(-1.0e8 - 50.0), &field));
  // Moderate magnitude (~1e6): +0.5 accepted, +2 refused.
  CHECK(!araSourceIdentityMismatch(withFingerprint(1.0e6),
                                   withFingerprint(1.0e6 + 0.5), &field));
  CHECK(araSourceIdentityMismatch(withFingerprint(1.0e6),
                                  withFingerprint(1.0e6 + 2.0), &field));

  std::cout << "fingerprint relative tolerance: ok\n";
}

void testUnknownFieldsNeverRefuse() {
  const auto stored = fullIdentity();

  // Live source unreadable / unmeasured: every field unknown.
  const AraSourceIdentity unknownLive;
  CHECK(!araSourceIdentityMismatch(stored, unknownLive));

  // The pre-v5 case: nothing was stored at all.
  const AraSourceIdentity legacyStored;
  CHECK(!araSourceIdentityMismatch(legacyStored, fullIdentity()));
  CHECK(!araSourceIdentityMismatch(legacyStored, unknownLive));

  // Only the ID known on both sides, and equal: length and
  // fingerprint being unknown must not refuse.
  AraSourceIdentity idOnlyStored;
  idOnlyStored.sourceId = "source-abc";
  AraSourceIdentity idOnlyLive;
  idOnlyLive.sourceId = "source-abc";
  idOnlyLive.sampleCount = 999;
  CHECK(!araSourceIdentityMismatch(idOnlyStored, idOnlyLive));

  std::cout << "unknown fields never refuse: ok\n";
}

void testStreamCodecRoundTrips() {
  const auto stored = fullIdentity();
  juce::MemoryOutputStream out;
  writeAraSourceIdentity(out, stored);
  const auto block = out.getMemoryBlock();
  juce::MemoryInputStream in(block, false);
  const auto readBack = readAraSourceIdentity(in);
  CHECK(readBack.sourceId == stored.sourceId);
  CHECK(readBack.sampleCount == stored.sampleCount);
  CHECK(readBack.fingerprint == stored.fingerprint);
  CHECK(readBack.hasFingerprint == stored.hasFingerprint);
  CHECK(in.isExhausted());

  // The empty record round-trips too (an unreadable source at store
  // time must not corrupt the stream layout).
  juce::MemoryOutputStream out2;
  writeAraSourceIdentity(out2, AraSourceIdentity{});
  const auto block2 = out2.getMemoryBlock();
  juce::MemoryInputStream in2(block2, false);
  const auto empty = readAraSourceIdentity(in2);
  CHECK(empty.sourceId.isEmpty());
  CHECK(empty.sampleCount == 0);
  CHECK(!empty.hasFingerprint);
  CHECK(in2.isExhausted());

  std::cout << "stream codec round-trips: ok\n";
}

} // namespace

int main() {
  testMatchingIdentitiesAreAccepted();
  testContentFieldMismatchRefuses();
  testSourceIdOnlyMismatchIsAccepted();
  testFingerprintRelativeTolerance();
  testUnknownFieldsNeverRefuse();
  testStreamCodecRoundTrips();
  std::cout << "Ara archive identity tests passed\n";
  return 0;
}
