#pragma once

#include "../JuceHeader.h"

#include <vector>

/**
 * Platform-specific path utilities.
 *
 * macOS:
 *   - Models: ~/Library/Application Support/FlexaTune/models/, then
 *             /Library/Application Support/FlexaTune/models/
 *             (batch 3 / 3.1 installs under .../Keinoaza/FlexaTune/ are
 *             still read as a fallback - see resourcesRootCandidates)
 *   - Logs: ~/Library/Logs/FlexaTune/
 *   - Config: ~/Library/FlexaTune/
 *
 * Windows:
 *   - Models: %APPDATA%/FlexaTune/models/, then
 *             %PROGRAMDATA%/FlexaTune/models/
 *             (batch 3 / 3.1 installs under .../Keinoaza/FlexaTune/ are
 *             still read as a fallback - see resourcesRootCandidates)
 *   - Logs: %APPDATA%/FlexaTune/Logs/
 *   - Config: %APPDATA%/FlexaTune/
 *
 * Linux:
 *   - Models: <exe_dir>/models/
 *   - Logs: ~/.config/FlexaTune/logs/
 *   - Config: ~/.config/FlexaTune/
 */
namespace PlatformPaths
{
    inline constexpr const char *kVendorFolderName = "Keinoaza";
    inline constexpr const char *kProductFolderName = "FlexaTune";

    // Batch 3.2: the resources root no longer nests under the vendor
    // folder - <base>/FlexaTune instead of <base>/Keinoaza/FlexaTune -
    // so shared data lives at %PROGRAMDATA%/FlexaTune on Windows.
    // Batch 3 / 3.1 installs keep their models under the legacy root,
    // so resolution prefers the new roots and falls back to the legacy
    // ones; the legacy folder is never moved or deleted.
    inline juce::File resourcesRootForBase(const juce::File &base,
                                           bool legacyHierarchy)
    {
        auto root = base;
        if (legacyHierarchy)
            root = root.getChildFile(kVendorFolderName);
        return root.getChildFile(kProductFolderName);
    }

    inline juce::File resourcesBaseFor(juce::File::SpecialLocationType location)
    {
        auto base = juce::File::getSpecialLocation(location);
#if JUCE_MAC
        base = base.getChildFile("Application Support");
#endif
        return base;
    }

    // Preference order: new hierarchy (per-user, then common), then
    // the legacy Keinoaza hierarchy (per-user, then common).
    inline std::vector<juce::File> resourcesRootCandidates()
    {
        const auto userBase =
            resourcesBaseFor(juce::File::userApplicationDataDirectory);
        const auto commonBase =
            resourcesBaseFor(juce::File::commonApplicationDataDirectory);
        return {resourcesRootForBase(userBase, false),
                resourcesRootForBase(commonBase, false),
                resourcesRootForBase(userBase, true),
                resourcesRootForBase(commonBase, true)};
    }

    inline std::vector<juce::File> legacyResourcesRootCandidates()
    {
        const auto userBase =
            resourcesBaseFor(juce::File::userApplicationDataDirectory);
        const auto commonBase =
            resourcesBaseFor(juce::File::commonApplicationDataDirectory);
        return {resourcesRootForBase(userBase, true),
                resourcesRootForBase(commonBase, true)};
    }

    inline juce::File
    pickResourcesDirectory(const std::vector<juce::File> &candidatesInPreferenceOrder,
                           const juce::File &fallback)
    {
        for (const auto &candidate : candidatesInPreferenceOrder)
            if (candidate.getChildFile("models").isDirectory())
                return candidate;
        return fallback;
    }

    inline juce::File getProjectResourcesDirectory()
    {
#if JUCE_DEBUG
#ifdef PITCHNET_PROJECT_RESOURCES_DIR
        auto configuredResources = juce::File(juce::String(PITCHNET_PROJECT_RESOURCES_DIR));
        if (configuredResources.isDirectory())
            return configuredResources;
#endif

        auto sourceRelativeProbe = juce::File(__FILE__)
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getParentDirectory()
                                       .getChildFile("Resources");
        if (sourceRelativeProbe.isDirectory())
            return sourceRelativeProbe;

        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources");
        if (cwdProbe.isDirectory())
            return cwdProbe;

        return {};
#else
        // FlexaTune keeps its resources apart from PitchNet's. The
        // per-user root comes first so a local install needs no
        // administrator rights; when neither new root has a models
        // folder yet, the legacy (batch 3 / 3.1) Keinoaza roots are
        // honoured so existing installs keep finding their models.
        return pickResourcesDirectory(
            resourcesRootCandidates(),
            resourcesRootForBase(
                resourcesBaseFor(juce::File::commonApplicationDataDirectory),
                false));
#endif
    }

    inline juce::File getApplicationFile()
    {
        return juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    }

    inline juce::File getDefaultLocalResourcesModelsDirectory()
    {
#if JUCE_MAC
        return getApplicationFile().getChildFile("Contents/Resources/models");
#else
        auto dir = getApplicationFile().getParentDirectory();
        const auto initialDir = dir;

        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            if (dir.getFileName().equalsIgnoreCase("Contents"))
                return dir.getChildFile("Resources/models");

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return initialDir.getChildFile("Resources/models");
#endif
    }

    inline juce::File findLocalResourcesModelsDirectory()
    {
        auto isValid = [](const juce::File &candidate)
        {
            return candidate.isDirectory();
        };

        auto projectResources = getProjectResourcesDirectory();
        auto projectModels = projectResources.getChildFile("models");
        if (isValid(projectModels))
            return projectModels;

        auto dir = getApplicationFile().getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto resourcesCandidate = dir.getChildFile("Resources/models");
            if (isValid(resourcesCandidate))
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return {};
    }

    inline juce::File getModelsDirectory()
    {
        auto localResourcesModels = findLocalResourcesModelsDirectory();
        if (localResourcesModels.isDirectory())
            return localResourcesModels;

        return getDefaultLocalResourcesModelsDirectory();
    }

    inline juce::File getModelFile(const juce::String &fileName)
    {
        auto probe = getModelsDirectory().getChildFile(fileName);
        if (probe.exists())
            return probe;

        // Development fallback: <repo>/Resources/models/
        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models")
                            .getChildFile(fileName);
        if (cwdProbe.exists())
            return cwdProbe;

        // Walk up from executable directory and probe both:
        //   <dir>/models/<file>
        //   <dir>/Resources/models/<file>
        auto dir = getApplicationFile().getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto modelsCandidate = dir.getChildFile("models").getChildFile(fileName);
            if (modelsCandidate.exists())
                return modelsCandidate;

            auto resourcesCandidate = dir.getChildFile("Resources/models").getChildFile(fileName);
            if (resourcesCandidate.exists())
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        // Batch 3.2: probe the legacy-hierarchy roots for this specific
        // file. getProjectResourcesDirectory() prefers the new roots
        // once their models folder exists, so a model that only exists
        // under the batch 3 / 3.1 Keinoaza hierarchy (e.g. placed there
        // by hand) would otherwise be missed.
        for (const auto &legacyRoot : legacyResourcesRootCandidates())
        {
            auto legacyCandidate = legacyRoot.getChildFile("models")
                                       .getChildFile(fileName);
            if (legacyCandidate.exists())
                return legacyCandidate;
        }

        // Default path used in production packaging.
        return probe;
    }

    inline juce::File getVocoderModelFile()
    {
#if JUCE_MAC
        auto coreMLModel = getModelFile("pc_nsf_hifigan.mlmodelc");
        if (coreMLModel.isDirectory())
            return coreMLModel;
#endif
        return getModelFile("pc_nsf_hifigan.onnx");
    }

    // BigVGAN v2 (MIT) is the speech build's AI resynthesis model. It is
    // generated by tools/convert_bigvgan.py or placed by hand; unlike the
    // models above it is not part of the base install, so callers must
    // check existsAsFile() - AI Resynthesis is offered only when present.
    inline juce::File getBigVganModelFile()
    {
        return getModelFile("bigvgan_v2_44khz_128band.onnx");
    }

    // The lower-opset (17) variant of the BigVGAN model, generated next
    // to the main model by tools/convert_bigvgan.py. The vocoder's
    // session ladder uses it for its middle rungs; it is optional, so
    // callers must check existsAsFile().
    inline juce::File getBigVganOpset17ModelFile()
    {
        return getModelFile("bigvgan_v2_44khz_128band_opset17.onnx");
    }

    inline juce::File getModelSubDir(const juce::String &dirName,
                                     const juce::String &verifyFile = "")
    {
        // Helper: check if candidate dir is valid
        auto isValid = [&](const juce::File &candidate) -> bool
        {
            if (!candidate.isDirectory())
                return false;
            if (verifyFile.isEmpty())
                return true;
            return candidate.getChildFile(verifyFile).existsAsFile();
        };

        auto probe = getModelsDirectory().getChildFile(dirName);
        if (isValid(probe))
            return probe;

        auto cwdProbe = juce::File::getCurrentWorkingDirectory()
                            .getChildFile("Resources/models")
                            .getChildFile(dirName);
        if (isValid(cwdProbe))
            return cwdProbe;

        auto dir = getApplicationFile().getParentDirectory();
        for (int i = 0; i < 8 && dir.exists(); ++i)
        {
            auto modelsCandidate = dir.getChildFile("models").getChildFile(dirName);
            if (isValid(modelsCandidate))
                return modelsCandidate;

            auto resourcesCandidate = dir.getChildFile("Resources/models").getChildFile(dirName);
            if (isValid(resourcesCandidate))
                return resourcesCandidate;

            auto parent = dir.getParentDirectory();
            if (parent == dir)
                break;
            dir = parent;
        }

        return probe;
    }

    inline juce::File getLogsDirectory()
    {
#if JUCE_MAC
        // macOS: ~/Library/Logs/FlexaTune/
        return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
            .getChildFile("Library/Logs")
            .getChildFile(kProductFolderName);
#elif JUCE_WINDOWS
        // Windows: %APPDATA%/FlexaTune/Logs/
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile(kProductFolderName)
            .getChildFile("Logs");
#else
        // Linux: ~/.config/FlexaTune/logs/
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile(kProductFolderName)
            .getChildFile("logs");
#endif
    }

    inline juce::File getConfigDirectory()
    {
        // All platforms use userApplicationDataDirectory
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile(kProductFolderName);
    }

    inline juce::File getLogFile(const juce::String &name)
    {
        auto logsDir = getLogsDirectory();
        logsDir.createDirectory();
        return logsDir.getChildFile(name);
    }

    inline juce::File getConfigFile(const juce::String &name)
    {
        auto configDir = getConfigDirectory();
        configDir.createDirectory();
        return configDir.getChildFile(name);
    }
}
