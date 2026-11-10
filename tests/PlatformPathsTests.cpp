// Batch 3.2 / 4 tests for the resources-directory resolution.
//
// The shared-data root is <base>/FlexaTune (on Windows:
// %PROGRAMDATA%\FlexaTune). Batch 3.2 moved it out of the vendor
// hierarchy (<base>/Keinoaza/FlexaTune) and batch 4 removed the read
// fallback to that hierarchy: the batch 4 installer places the models
// at the current root and deletes the legacy FlexaTune folder. The
// helpers under test are the exact ones
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
  // --- Root shape: no vendor level anywhere.
  const juce::File base("/base");
  const auto root = PlatformPaths::resourcesRootForBase(base);
  CHECK(root.getFileName() == "FlexaTune");
  CHECK(root.getParentDirectory().getFileName() != "Keinoaza");

  // --- The real candidate list is exactly the per-user root and the
  // common root, in that order (whatever the platform's special
  // locations are here).
  const auto candidates = PlatformPaths::resourcesRootCandidates();
  CHECK(candidates.size() == 2);
  CHECK(candidates[0].getFileName() == "FlexaTune");
  CHECK(candidates[0].getParentDirectory().getFileName() != "Keinoaza");
  CHECK(candidates[1].getFileName() == "FlexaTune");
  CHECK(candidates[1].getParentDirectory().getFileName() != "Keinoaza");

  // --- Preference order against a synthetic directory tree, in the
  // same order resourcesRootCandidates() produces: user, common.
  auto tempRoot = juce::File::getSpecialLocation(
                      juce::File::SpecialLocationType::tempDirectory)
                      .getChildFile("FlexaTunePathsTests");
  tempRoot.deleteRecursively();
  tempRoot.createDirectory();

  const auto user = tempRoot.getChildFile("user");     // no models yet
  auto common = makeModelsRoot(tempRoot, "common");    // has models
  const std::vector<juce::File> order = {user, common};
  const juce::File fallback = common;

  // Only the common root has models: it is picked.
  CHECK(PlatformPaths::pickResourcesDirectory(order, fallback) == common);

  // A per-user root with models wins over the common root.
  auto userWithModels = makeModelsRoot(tempRoot, "user");
  CHECK(userWithModels == user);
  CHECK(PlatformPaths::pickResourcesDirectory(order, fallback) == user);

  // Nothing has models anywhere: the fallback (the common root) is
  // returned so first-run creation lands in the current hierarchy.
  const std::vector<juce::File> emptyOrder = {
      tempRoot.getChildFile("noneA"), tempRoot.getChildFile("noneB")};
  CHECK(PlatformPaths::pickResourcesDirectory(emptyOrder, fallback) ==
        fallback);

  tempRoot.deleteRecursively();

  std::cout << "PlatformPathsTests passed" << std::endl;
  return 0;
}
