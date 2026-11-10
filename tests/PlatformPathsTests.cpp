// Batch 3.2 tests for the resources-directory resolution.
//
// The shared-data root moved from <base>/Keinoaza/FlexaTune to
// <base>/FlexaTune (on Windows: %PROGRAMDATA%\FlexaTune). Resolution
// must prefer the new hierarchy while batch 3 / 3.1 installs, whose
// models only exist under the legacy Keinoaza hierarchy, keep working
// through the fallback. The helpers under test are the exact ones
// PlatformPaths::getProjectResourcesDirectory() uses.
#include "../Source/Utils/PlatformPaths.h"
#include "TestAssert.h"
#include <iostream>

namespace {

juce::File makeModelsRoot(const juce::File &parent, const juce::String &name)
{
  auto root = parent.getChildFile(name);
  root.getChildFile("models").createDirectory();
  return root;
}

} // namespace

int main()
{
  // --- Root shape: the new hierarchy drops the vendor level, the
  // legacy one keeps it.
  const juce::File base("/base");
  const auto newRoot = PlatformPaths::resourcesRootForBase(base, false);
  CHECK(newRoot.getFileName() == "FlexaTune");
  CHECK(newRoot.getParentDirectory().getFileName() != "Keinoaza");
  const auto legacyRoot = PlatformPaths::resourcesRootForBase(base, true);
  CHECK(legacyRoot.getFileName() == "FlexaTune");
  CHECK(legacyRoot.getParentDirectory().getFileName() == "Keinoaza");

  // --- The real candidate list keeps that shape (whatever the
  // platform's special locations are here).
  const auto candidates = PlatformPaths::resourcesRootCandidates();
  CHECK(candidates.size() == 4);
  CHECK(candidates[0].getFileName() == "FlexaTune");
  CHECK(candidates[0].getParentDirectory().getFileName() != "Keinoaza");
  CHECK(candidates[1].getFileName() == "FlexaTune");
  CHECK(candidates[1].getParentDirectory().getFileName() != "Keinoaza");
  CHECK(candidates[2].getParentDirectory().getFileName() == "Keinoaza");
  CHECK(candidates[3].getParentDirectory().getFileName() == "Keinoaza");
  const auto legacyCandidates = PlatformPaths::legacyResourcesRootCandidates();
  CHECK(legacyCandidates.size() == 2);
  CHECK(legacyCandidates[0].getParentDirectory().getFileName() == "Keinoaza");

  // --- Preference order against a synthetic directory tree, in the
  // same order resourcesRootCandidates() produces: user-new,
  // common-new, user-legacy, common-legacy.
  auto tempRoot = juce::File::getSpecialLocation(
                      juce::File::SpecialLocationType::tempDirectory)
                      .getChildFile("FlexaTunePathsTests");
  tempRoot.deleteRecursively();
  tempRoot.createDirectory();

  const auto userNew = tempRoot.getChildFile("userNew");       // no models yet
  const auto commonNew = tempRoot.getChildFile("commonNew");   // no models yet
  const auto userLegacy = tempRoot.getChildFile("userLegacy"); // no models yet
  auto commonLegacy = makeModelsRoot(tempRoot, "commonLegacy");
  const std::vector<juce::File> order = {userNew, commonNew, userLegacy,
                                         commonLegacy};
  const juce::File fallback = commonNew;

  // Only the legacy common root has models (a batch 3 / 3.1 install):
  // resolution falls back to it.
  CHECK(PlatformPaths::pickResourcesDirectory(order, fallback) ==
        commonLegacy);

  // The new common root gains models (batch 3.2 install): it wins
  // over the legacy root.
  auto commonNewWithModels = makeModelsRoot(tempRoot, "commonNew");
  CHECK(commonNewWithModels == commonNew);
  CHECK(PlatformPaths::pickResourcesDirectory(order, fallback) == commonNew);

  // A per-user new root wins over both common roots.
  auto userNewWithModels = makeModelsRoot(tempRoot, "userNew");
  CHECK(userNewWithModels == userNew);
  CHECK(PlatformPaths::pickResourcesDirectory(order, fallback) == userNew);

  // Nothing has models anywhere: the fallback (new common root) is
  // returned so first-run creation lands in the new hierarchy.
  const std::vector<juce::File> emptyOrder = {
      tempRoot.getChildFile("noneA"), tempRoot.getChildFile("noneB"),
      tempRoot.getChildFile("noneC"), tempRoot.getChildFile("noneD")};
  CHECK(PlatformPaths::pickResourcesDirectory(emptyOrder, fallback) ==
        fallback);

  tempRoot.deleteRecursively();

  std::cout << "PlatformPathsTests passed" << std::endl;
  return 0;
}
