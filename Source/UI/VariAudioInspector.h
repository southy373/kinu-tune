// Kinu Tune - Copyright (C) 2026 Keinoaza
// Part of Kinu Tune, a modified version of PitchNet. Licensed under the GNU
// Affero General Public License, version 3. See LICENSE and MODIFICATIONS.md.

#pragma once

#include "../JuceHeader.h"
#include "../Models/Project.h"
#include "../Utils/Localization.h"
#include "Components/StyledWidgets.h"
#include "Sliders.h"
#include "HarmonyPanel.h"
#include <array>
#include <functional>
#include <optional>

// Always-visible inspector on the left of the editor, laid out like the
// VariAudio section of Cubase's Sample Editor inspector: smart-control and
// pitch-snap menus, then the segment sliders (Correct Pitch, Flatten Curve,
// Pitch Drift, Shift Formant, Volume), then the editor scale (root and scale
// menus), then the harmony buttons. Scrolls vertically when the editor is
// shorter than the column.
class VariAudioInspector : public juce::Component, private juce::Timer
{
public:
    static constexpr int preferredWidth = 240;

    // Slider order matches PianoRollComponent::InspectorParam. None is
    // the global faders' marker: they are ValueRows too (batch 4) but
    // belong to no segment parameter.
    enum class Param { CorrectPitch, FlattenCurve, PitchDrift, Formant, Volume, None };
    // VariAudio Pitch Snap Mode: Absolute / Relative / Off.
    enum class SnapMode { Absolute, Relative, Off };

    VariAudioInspector();
    ~VariAudioInspector() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void refreshLocalisedText();
    /** Re-reads snap state and slider values now (they are also polled). */
    void refresh() { refreshFromProject(); }

    HarmonyPanel& getHarmonyPanel() { return harmonyPanel; }

    // Asked on every refresh; never cached (see HarmonyPanel).
    std::function<Project*()> getCurrentProject;
    std::function<void(SnapMode)> onSnapModeChanged;
    std::function<void(DragSnapMode)> onDragSnapModeChanged;
    std::function<void(bool)> onShowAllSmartControlsChanged;

    // Editor scale. The menus show the key the current region uses (the one
    // the harmony buttons and the piano roll follow). Picking a key in clip
    // view gives the region its own key; in track view a region that follows
    // the project key changes the project key, shared by every such region.
    // The "Use Project Key" switch makes the region follow it again.
    std::function<bool()> isProjectKeyScope;
    std::function<void()> onEditorScaleChanged;

    // Segment sliders. getParamValue returns nullopt when nothing can be
    // edited; getTargetCount says how many segments a slider will change and
    // whether they are the selection (true) or every segment (false).
    std::function<std::optional<float>(Param)> getParamValue;
    std::function<std::pair<int, bool>()> getTargetCount;
    std::function<void(Param)> onParamEditBegin;
    std::function<void(float)> onParamEditValue;
    std::function<void()> onParamEditEnd;

    // Project-wide faders (batch 3: moved here from the parameter panel,
    // to the top of this column). Commits only: a drag reports on drag
    // end; a typed value or an Alt+click reset reports at once.
    std::function<void(double)> onGlobalPitchPercentChanged;
    std::function<void(double)> onGlobalFormantChanged;
    std::function<void(double)> onGlobalLengthPercentChanged;
    /** Re-reads the three fader positions from the current project. */
    void updateGlobalSliders();

private:
    class ComboLookAndFeel; // shrinks long scale names to fit the menu

    // Scrolled content; it paints the card backgrounds.
    class Content : public juce::Component
    {
    public:
        explicit Content(VariAudioInspector& o) : owner(o) {}
        void paint(juce::Graphics& g) override { owner.paintContent(g); }

    private:
        VariAudioInspector& owner;
    };

    // Cubase-style value row: caption left, value right, thin bar below.
    // One component for every fader in this column since batch 4: the
    // five segment rows format through their Param, the three global
    // rows through an explicit formatter and additionally carry the
    // global faders' own interactions (double-click numeric entry,
    // Alt+click reset to neutral).
    class ValueRow : public juce::Slider
    {
    public:
        ValueRow(Param param, double minimum, double maximum, double defaultValue,
                 double step);
        ValueRow(std::function<juce::String(double)> formatter, double minimum,
                 double maximum, double neutralValue, double step);
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDoubleClick(const juce::MouseEvent& e) override;
        juce::String caption;
        const Param param;
        const double defaultValue;
        std::function<juce::String(double)> valueFormatter;
        bool globalInteractions = false;

    private:
        void openNumericEntry();
    };

    void timerCallback() override;
    void refreshFromProject();
    void refreshEditorScale(Project* project);
    void applyScaleRoot(int root);
    void applyScaleMode(ScaleMode mode);
    void paintContent(juce::Graphics& g);
    void layoutContent(int width);
    int getContentHeight() const;
    bool projectKeyScope() const { return isProjectKeyScope && isProjectKeyScope(); }
    static void showMenuValue(juce::ComboBox& box, int id, int& shownId);
    void setSnapModeInternal(SnapMode mode, bool notify);
    static juce::String formatValue(Param param, double value);
    void setGlobalPitchPercentInternal(double percent, bool notify);
    void setGlobalFormantInternal(double semitones, bool notify);
    void setGlobalLengthPercentInternal(double percent, bool notify);

    // Outlives the menus that use it (declared first, destroyed last).
    std::unique_ptr<ComboLookAndFeel> comboLookAndFeel;
    juce::Viewport viewport;
    Content content{*this};

    // Global card (batch 3): the project-wide faders, above everything.
    // Batch 4: the same ValueRow component as the segment sliders
    // below, so look and operation match; they keep their commit-only
    // reporting and their own double-click / Alt+click interactions.
    juce::Label globalHeader;
    std::unique_ptr<ValueRow> globalPitchRow;
    std::unique_ptr<ValueRow> globalFormantRow;
    std::unique_ptr<ValueRow> globalLengthRow;
    double globalPitchPercent = 100.0;
    double globalFormantShift = 0.0;
    double globalLengthPercent = 100.0;
    bool globalPitchDragging = false;
    bool globalFormantDragging = false;
    bool globalLengthDragging = false;
    bool globalIsUpdating = false;
    juce::Rectangle<int> globalCardBounds;

    juce::Label variAudioHeader;
    juce::Label smartControlsLabel;
    juce::ComboBox smartControlsBox;
    juce::Label snapModeLabel;
    juce::ComboBox snapModeBox;
    juce::Label targetLabel;
    std::array<std::unique_ptr<ValueRow>, 5> rows;
    bool editingRow = false;
    bool absoluteSelected = true; // Absolute unless the user picks Relative
    juce::Rectangle<int> variAudioCardBounds;
    juce::Rectangle<int> rowsBoxBounds;

    juce::Label editorScaleHeader;
    StyledToggleButton useProjectKeyToggle;
    juce::ComboBox scaleRootBox;
    juce::ComboBox scaleModeBox;
    StyledToggleButton snapScaleToggle; // VariAudio "Snap Pitch Editing"
    juce::Rectangle<int> editorScaleCardBounds;
    // Menu ids last written by refresh. A menu showing anything else holds a
    // pick whose (asynchronous) onChange has not run yet; refresh must not
    // overwrite it, or onChange would read the old value back.
    int shownSnapModeId = 0;
    int shownRootId = 0;
    int shownModeId = 0;

    HarmonyPanel harmonyPanel; // draws its own card and title

    LocalisationWatcher languageWatcher{[this] { refreshLocalisedText(); }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VariAudioInspector)
};
