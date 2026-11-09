#pragma once

#include "../JuceHeader.h"

/**
 * Platform-specific path utilities.
 *
 * macOS:
 *   - Models: ~/Library/Application Support/Keinoaza/FlexaTune/models/, then
 *             /Library/Application Support/Keinoaza/FlexaTune/models/
 *   - Logs: ~/Library/Logs/FlexaTune/
 *   - Config: ~/Library/FlexaTune/
 *
 * Windows:
 *   - Models: %APPDATA%/Keinoaza/FlexaTune/models/, then
 *             %PROGRAMDATA%/Keinoaza/FlexaTune/models/
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
        // FlexaTune keeps its resources apart from PitchNet's. The per-user
        // folder comes first so a local install needs no administrator rights.
        auto resourcesUnder = [](juce::File::SpecialLocationType location)
        {
            return juce::File::getSpecialLocation(location)
#if JUCE_MAC
                .getChildFile("Application Support")
#endif
                .getChildFile(kVendorFolderName)
                .getChildFile(kProductFolderName);
        };

        auto userResources = resourcesUnder(juce::File::userApplicationDataDirectory);
        if (userResources.getChildFile("models").isDirectory())
            return userResources;
        return resourcesUnder(juce::File::commonApplicationDataDirectory);
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
