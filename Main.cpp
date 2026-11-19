#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <array>
#include <deque>
#include <atomic>
#include <cmath>
#include <functional>
#include <tuple>

using namespace juce;
namespace {
constexpr int startingTracks = 4, steps = 16, block = 256, rowHeight = 72;
constexpr auto navy = 0xff101828, card = 0xff1a2639, cyan = 0xff69dce9;

struct Clock : AudioPlayHead {
    PositionInfo position;
    Optional<PositionInfo> getPosition() const override { return position; }
};

struct Clip {
    File file;
    AudioBuffer<float> audio;
    double rate = 44100;
};

struct Track {
    std::unique_ptr<Clip> clip;
    std::unique_ptr<AudioPluginInstance> plugin;
    PluginDescription description;
    AudioBuffer<float> audio { 2, block };
    MidiBuffer midi;
    std::array<bool, steps> pattern {};
    double phase = 0;
    float gain = 0.75f, envelope = 0;
    int note = 60, voice = -1, sequenced = -1;
    bool mute = false;
};

class Engine final : public AudioIODeviceCallback, public MidiInputCallback {
public:
    std::deque<Track> tracks;
    CriticalSection lock;
    MidiMessageCollector input;
    Clock clock;
    double rate = 44100, bpm = 120, position = 0;
    int selected = 0;
    bool playing = false;
    std::atomic<int> playStep { -1 };

    void audioDeviceAboutToStart(AudioIODevice* device) override {
        const ScopedLock guard(lock);
        rate = device->getCurrentSampleRate();
        if (rate <= 0) rate = 44100;
        input.reset(rate);
        for (auto& track : tracks)
            if (track.plugin) {
                track.plugin->releaseResources();
                prepare(*track.plugin);
            }
    }

    void audioDeviceStopped() override {
        const ScopedLock guard(lock);
        for (auto& track : tracks)
            if (track.plugin) track.plugin->releaseResources();
    }

    void prepare(AudioPluginInstance& plugin) {
        plugin.setPlayHead(&clock);
        plugin.setRateAndBufferSizeDetails(rate, block);
        plugin.prepareToPlay(rate, block);
    }

    void handleIncomingMidiMessage(MidiInput*, const MidiMessage& message) override {
        if (message.getTimeStamp() > 0) input.addMessageToQueue(message);
    }

    void audioDeviceIOCallbackWithContext(const float* const*, int,
                                          float* const* output, int channels, int count,
                                          const AudioIODeviceCallbackContext&) override {
        for (int ch = 0; ch < channels; ++ch)
            if (output[ch]) FloatVectorOperations::clear(output[ch], count);
        const ScopedTryLock guard(lock);
        if (!guard.isLocked()) return;
        for (int offset = 0; offset < count; offset += block) {
            const int size = jmin(block, count - offset);
            live.clear();
            input.removeNextBlockOfMessages(live, size);
            render(size, &live);
            for (int ch = 0; ch < jmin(2, channels); ++ch)
                if (output[ch]) FloatVectorOperations::copy(output[ch] + offset, mix.getReadPointer(ch), size);
        }
    }

    void releaseNotes() { panic = true; }

    void stop() {
        playing = false;
        position = 0;
        playStep = -1;
        panic = true;
    }

    bool bounce(const File& path) {
        const ScopedLock guard(lock);
        const auto total = static_cast<int>(std::ceil(steps * rate * 15.0 / bpm));
        TemporaryFile temp(path);
        std::unique_ptr<OutputStream> stream = temp.getFile().createOutputStream();
        if (!stream) return false;
        WavAudioFormat format;
        std::unique_ptr<AudioFormatWriter> writer(format.createWriterFor(stream.get(), rate, 2, 24, {}, 0));
        if (!writer) return false;
        stream.release();
        const bool resume = playing;
        const double original = position;
        stop();
        playing = true;
        for (auto& track : tracks) {
            track.phase = 0;
            track.envelope = 0;
            if (track.plugin) { track.plugin->reset(); track.plugin->setNonRealtime(true); }
        }
        bool ok = true;
        for (int offset = 0; offset < total && ok; offset += block) {
            const int count = jmin(block, total - offset);
            render(count, nullptr);
            ok = writer->writeFromAudioSampleBuffer(mix, 0, count);
        }
        writer.reset();
        for (auto& track : tracks)
            if (track.plugin) { track.plugin->setNonRealtime(false); track.plugin->reset(); }
        stop();
        playing = resume;
        position = original;
        return ok && temp.overwriteTargetFileWithTemporary();
    }

private:
    AudioBuffer<float> mix { 2, block };
    MidiBuffer live;
    bool panic = true;

    void render(int count, const MidiBuffer* incoming) {
        mix.setSize(2, count, false, false, true);
        mix.clear();
        const auto samplesPerStep = rate * 15.0 / bpm;
        const auto firstPosition = position;
        const int firstStep = static_cast<int>(position / samplesPerStep) % steps;
        AudioPlayHead::PositionInfo info;
        info.setBpm(bpm);
        info.setIsPlaying(playing);
        info.setTimeInSamples(static_cast<int64>(position));
        info.setTimeInSeconds(position / rate);
        info.setPpqPosition(position / (samplesPerStep * 4.0));
        info.setTimeSignature(AudioPlayHead::TimeSignature { 4, 4 });
        clock.position = info;
        for (int lane = 0; lane < static_cast<int>(tracks.size()); ++lane) {
            auto& track = tracks[static_cast<size_t>(lane)];
            track.midi.clear();
            if (panic) {
                for (int ch = 1; ch <= 16; ++ch) track.midi.addEvent(MidiMessage::allNotesOff(ch), 0);
                track.voice = -1;
                track.sequenced = -1;
            }
            if (incoming && lane == selected) track.midi.addEvents(*incoming, 0, count, 0);
            for (int sample = 0; sample < count; ++sample) {
                const auto stepPosition = (firstPosition + sample) / samplesPerStep;
                const int step = static_cast<int>(stepPosition) % steps;
                const int wanted = playing && track.pattern[static_cast<size_t>(step)]
                    && stepPosition - std::floor(stepPosition) < 0.72 ? track.note : -1;
                if (wanted != track.sequenced) {
                    if (track.sequenced >= 0) track.midi.addEvent(MidiMessage::noteOff(1, track.sequenced), sample);
                    if (wanted >= 0) track.midi.addEvent(MidiMessage::noteOn(1, wanted, uint8(100)), sample);
                    track.sequenced = wanted;
                }
            }
            auto* plugin = track.plugin.get();
            const int channelCount = plugin ? jmax(2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()) : 2;
            track.audio.setSize(channelCount, count, false, false, true);
            track.audio.clear();
            if (!plugin || (!track.description.isInstrument && plugin->getTotalNumInputChannels() > 0)) {
                if (track.clip) renderClip(track, count, samplesPerStep * steps);
                else renderTone(track, count);
            }
            if (plugin) plugin->processBlock(track.audio, track.midi);
            if (!track.mute)
                for (int ch = 0; ch < 2; ++ch) {
                    const int source = plugin && plugin->getTotalNumOutputChannels() == 1 ? 0 : ch;
                    mix.addFrom(ch, 0, track.audio, source, 0, count, track.gain * 0.35f);
                }
        }
        for (int ch = 0; ch < 2; ++ch) {
            auto* samples = mix.getWritePointer(ch);
            for (int sample = 0; sample < count; ++sample)
                samples[sample] = std::isfinite(samples[sample]) ? jlimit(-1.0f, 1.0f, samples[sample]) : 0.0f;
        }
        panic = false;
        if (playing) {
            playStep = firstStep;
            position = std::fmod(position + count, samplesPerStep * steps);
        }
    }

    void renderClip(Track& track, int count, double loopLength) {
        if (!playing) return;
        const auto& clip = *track.clip;
        const int length = clip.audio.getNumSamples();
        for (int sample = 0; sample < count; ++sample) {
            const auto source = std::fmod(position + sample, loopLength) * clip.rate / rate;
            if (source >= length) continue;
            const auto index = static_cast<int>(source);
            const auto next = jmin(index + 1, length - 1);
            const auto fraction = static_cast<float>(source - index);
            for (int ch = 0; ch < 2; ++ch) {
                const auto* data = clip.audio.getReadPointer(jmin(ch, clip.audio.getNumChannels() - 1));
                track.audio.setSample(ch, sample, data[index] + fraction * (data[next] - data[index]));
            }
        }
    }

    void renderTone(Track& track, int count) {
        auto event = track.midi.begin();
        const auto end = track.midi.end();
        auto* left = track.audio.getWritePointer(0);
        auto* right = track.audio.getWritePointer(1);
        for (int sample = 0; sample < count; ++sample) {
            while (event != end && (*event).samplePosition == sample) {
                const auto message = (*event).getMessage();
                if (message.isNoteOn()) track.voice = message.getNoteNumber();
                else if (message.isAllNotesOff() || (message.isNoteOff() && message.getNoteNumber() == track.voice))
                    track.voice = -1;
                ++event;
            }
            const float target = track.voice < 0 ? 0.0f : 0.42f;
            track.envelope += (target - track.envelope) * 0.012f;
            if (track.voice >= 0)
                track.phase = std::fmod(track.phase + MathConstants<double>::twoPi
                    * MidiMessage::getMidiNoteInHertz(track.voice) / rate, MathConstants<double>::twoPi);
            left[sample] = right[sample] = static_cast<float>(std::sin(track.phase)) * track.envelope;
        }
    }
};

class EditorWindow : public DocumentWindow {
public:
    EditorWindow(const String& name, AudioProcessorEditor* editor)
        : DocumentWindow(name, Colour(card), closeButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override { setVisible(false); }
};

class Studio final : public Component, private Timer {
public:
    Studio() {
        setLookAndFeel(&style);
        style.setColour(TextButton::buttonColourId, Colour(card));
        style.setColour(TextButton::buttonOnColourId, Colour(cyan).darker(0.48f));
        formats.addDefaultFormats();
        audioFormats.registerBasicFormats();
        addAndMakeVisible(title);
        title.setText("VOG STUDIO", dontSendNotification);
        title.setFont(FontOptions(27.0f, Font::bold));
        title.setColour(Label::textColourId, Colour(cyan));
        addAndMakeVisible(status);
        status.setText("Paint a pattern, then press Play. Each track starts with an internal tone.", dontSendNotification);
        addAndMakeVisible(tempo);
        tempo.setRange(5, 300, 1);
        tempo.setTextValueSuffix(" BPM");
        tempo.setValue(120);
        tempo.onValueChange = [this] {
            const ScopedLock guard(engine.lock);
            engine.bpm = tempo.getValue();
            engine.stop();
        };
        button(play, "Play", [this] { const ScopedLock guard(engine.lock); engine.playing = true; });
        button(stop, "Stop", [this] { const ScopedLock guard(engine.lock); engine.stop(); });
        button(settings, "Audio / MIDI", [this] { showSettings(); });
        button(open, "Open", [this] { openProject(); });
        button(save, "Save", [this] { saveProject(); });
        button(wav, "Export WAV", [this] {
            choose("Export one-bar mix", "*.wav", true, [this](File file) {
                report(engine.bounce(file.withFileExtension(".wav")) ? "WAV mix exported." : "WAV export failed.");
            });
        });
        button(midi, "Export MIDI", [this] { exportMidi(); });

        button(addTrackButton, "Add Track", [this] { addTrack(); });
        button(scanButton, "Scan VST3", [this] { beginScan(engine.selected); });
        addAndMakeVisible(trackViewport);
        trackViewport.setScrollBarsShown(true, false);
        trackViewport.setViewedComponent(&trackPanel, false);
        for (int i = 0; i < startingTracks; ++i) {
            engine.tracks.emplace_back().note = std::array<int, startingTracks>{ 48, 55, 60, 67 }[static_cast<size_t>(i)];
            setupRow(i);
        }
        for (int i = 0; i < steps; ++i) {
            auto& label = counts[static_cast<size_t>(i)];
            addAndMakeVisible(label);
            label.setJustificationType(Justification::centred);
            label.setText(String(i + 1), dontSendNotification);
        }
        setSize(1180, 490);
        engine.input.reset(engine.rate);
        const auto error = devices.initialise(0, 2, nullptr, true);
        devices.addAudioCallback(&engine);
        devices.addMidiInputDeviceCallback({}, &engine);
        if (error.isNotEmpty()) report(error);
        refresh();
        startTimerHz(20);
    }

    ~Studio() override {
        stopTimer();
        chooser.reset();
        scanner.reset();
        trackViewport.setViewedComponent(nullptr);
        settingsWindow = nullptr;
        editor.reset();
        devices.removeMidiInputDeviceCallback({}, &engine);
        devices.removeAudioCallback(&engine);
        for (auto& track : engine.tracks) track.plugin.reset();
        setLookAndFeel(nullptr);
    }

    void paint(Graphics& g) override { g.fillAll(Colour(navy)); }

    void resized() override {
        auto header = getLocalBounds().reduced(20, 12).removeFromTop(36);
        title.setBounds(header.removeFromLeft(198));
        for (auto* control : { &play, &stop, &settings, &open, &save, &wav, &midi })
            control->setBounds(header.removeFromLeft(96).reduced(3, 1));
        tempo.setBounds(header.reduced(5, 1));
        addTrackButton.setBounds(20, 56, 105, 28);
        scanButton.setBounds(131, 56, 105, 28);
        trackViewport.setBounds(18, 118, getWidth() - 36, getHeight() - 174);
        trackPanel.setSize(trackViewport.getWidth() - trackViewport.getScrollBarThickness(),
                           static_cast<int>(rows.size()) * rowHeight);
        const int gridLeft = 178;
        const int gridWidth = jmax(160, trackPanel.getWidth() - gridLeft - 201);
        for (int step = 0; step < steps; ++step) {
            const int x = gridLeft + step * gridWidth / steps;
            const int width = gridLeft + (step + 1) * gridWidth / steps - x;
            counts[static_cast<size_t>(step)].setBounds(x + trackViewport.getX(), 89, width, 24);
            for (int i = 0; i < static_cast<int>(rows.size()); ++i)
                rows[static_cast<size_t>(i)].pads[static_cast<size_t>(step)].setBounds(x + 2, 10 + i * rowHeight, width - 4, 44);
        }
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            auto& row = rows[static_cast<size_t>(i)];
            const int y = 4 + i * rowHeight;
            row.select.setBounds(8, y + 4, 70, 26);
            row.load.setBounds(82, y + 4, 88, 26);
            row.editor.setBounds(8, y + 34, 162, 24);
            row.pitch.setBounds(trackPanel.getWidth() - 192, y + 3, 136, 27);
            row.level.setBounds(trackPanel.getWidth() - 192, y + 34, 136, 24);
            row.mute.setBounds(trackPanel.getWidth() - 53, y + 4, 45, 26);
        }
        status.setBounds(20, getHeight() - 56, getWidth() - 40, 36);
    }

private:
    struct TrackPanel : Component {
        void paint(Graphics& g) override {
            g.setColour(Colour(card));
            const auto visible = g.getClipBounds();
            for (int y = (visible.getY() / rowHeight) * rowHeight; y < visible.getBottom(); y += rowHeight)
                g.fillRoundedRectangle(0.0f, static_cast<float>(y), static_cast<float>(getWidth()), 64.0f, 9.0f);
        }
    };

    struct Row {
        TextButton select, load, editor, mute;
        Slider pitch { Slider::LinearHorizontal, Slider::TextBoxLeft };
        Slider level { Slider::LinearHorizontal, Slider::TextBoxLeft };
        std::array<TextButton, steps> pads;
    };
    LookAndFeel_V4 style;
    AudioPluginFormatManager formats;
    AudioFormatManager audioFormats;
    Engine engine;
    AudioDeviceManager devices;
    TrackPanel trackPanel;
    Viewport trackViewport;
    std::deque<Row> rows;
    std::array<Label, steps> counts;
    Label title, status;
    Slider tempo { Slider::LinearHorizontal, Slider::TextBoxRight };
    TextButton play, stop, settings, open, save, wav, midi, addTrackButton, scanButton;
    KnownPluginList vst3Catalog;
    std::unique_ptr<PluginDirectoryScanner> scanner;
    FileSearchPath extraVst3Paths;
    int scanTrack = 0;
    Component::SafePointer<DialogWindow> settingsWindow;
    std::unique_ptr<EditorWindow> editor;
    std::unique_ptr<FileChooser> chooser;
    int shownStep = -2;

    void report(const String& message) { status.setText(message, dontSendNotification); }

    void button(TextButton& widget, const String& label, std::function<void()> action, Component* parent = nullptr) {
        (parent ? *parent : *this).addAndMakeVisible(widget);
        widget.setButtonText(label);
        widget.onClick = std::move(action);
    }

    void addTrack() {
        const auto index = static_cast<int>(rows.size());
        {
            const ScopedLock guard(engine.lock);
            engine.tracks.emplace_back();
            engine.selected = index;
            engine.releaseNotes();
        }
        setupRow(index);
        resized();
        refresh();
        trackViewport.setViewPosition(0, index * rowHeight);
        report("Added track " + String(index + 1) + ".");
    }

    void setupRow(int i) {
        auto& row = rows.emplace_back();
        button(row.select, "Track " + String(i + 1), [this, i] {
            { const ScopedLock guard(engine.lock);
              if (engine.selected != i) { engine.selected = i; engine.releaseNotes(); } }
            refresh();
        }, &trackPanel);
        button(row.load, "Source / FX…", [this, i] { pluginMenu(i); }, &trackPanel);
        button(row.editor, "Internal tone", [this, i] { showEditor(i); }, &trackPanel);
        button(row.mute, "Mute", [this, i] {
            const ScopedLock guard(engine.lock);
            auto& t = engine.tracks[static_cast<size_t>(i)];
            t.mute = !t.mute;
            rows[static_cast<size_t>(i)].mute.setToggleState(t.mute, dontSendNotification);
        }, &trackPanel);
        trackPanel.addAndMakeVisible(row.pitch);
        row.pitch.setRange(0, 127, 1);
        row.pitch.setTextBoxStyle(Slider::TextBoxLeft, false, 48, 21);
        row.pitch.textFromValueFunction = [](double note) {
            return MidiMessage::getMidiNoteName(roundToInt(note), true, true, 4);
        };
        row.pitch.valueFromTextFunction = [](const String& name) {
            for (int note = 0; note < 128; ++note)
                if (MidiMessage::getMidiNoteName(note, true, true, 4).equalsIgnoreCase(name.trim()))
                    return static_cast<double>(note);
            return name.getDoubleValue();
        };
        row.pitch.onValueChange = [this, i] {
            const ScopedLock guard(engine.lock);
            engine.tracks[static_cast<size_t>(i)].note = roundToInt(rows[static_cast<size_t>(i)].pitch.getValue());
        };
        trackPanel.addAndMakeVisible(row.level);
        row.level.setRange(0, 1, 0.01);
        row.level.setTextBoxStyle(Slider::TextBoxLeft, false, 48, 21);
        row.level.onValueChange = [this, i] {
            const ScopedLock guard(engine.lock);
            engine.tracks[static_cast<size_t>(i)].gain = static_cast<float>(rows[static_cast<size_t>(i)].level.getValue());
        };
        for (int step = 0; step < steps; ++step)
            button(row.pads[static_cast<size_t>(step)], {}, [this, i, step] {
                const ScopedLock guard(engine.lock);
                auto& active = engine.tracks[static_cast<size_t>(i)].pattern[static_cast<size_t>(step)];
                active = !active;
                rows[static_cast<size_t>(i)].pads[static_cast<size_t>(step)].setToggleState(active, dontSendNotification);
            }, &trackPanel);
    }

    void timerCallback() override {
        if (scanner) advanceScan();
        const auto step = engine.playStep.load();
        if (step == shownStep) return;
        shownStep = step;
        for (int i = 0; i < steps; ++i)
            counts[static_cast<size_t>(i)].setColour(Label::textColourId,
                Colour(i == step ? cyan : 0xffa0b3c9));
    }

    void refresh() {
        const ScopedLock guard(engine.lock);
        tempo.setValue(engine.bpm, dontSendNotification);
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            auto& row = rows[static_cast<size_t>(i)];
            const auto& track = engine.tracks[static_cast<size_t>(i)];
            row.select.setToggleState(i == engine.selected, dontSendNotification);
            row.mute.setToggleState(track.mute, dontSendNotification);
            row.pitch.setValue(track.note, dontSendNotification);
            row.level.setValue(track.gain, dontSendNotification);
            row.editor.setButtonText(track.plugin ? track.description.name :
                track.clip ? track.clip->file.getFileNameWithoutExtension() : "Internal tone");
            for (int step = 0; step < steps; ++step)
                row.pads[static_cast<size_t>(step)].setToggleState(track.pattern[static_cast<size_t>(step)], dontSendNotification);
        }
    }

    void choose(const String& prompt, const String& wildcard, bool writing, std::function<void(File)> action, bool folder = false) {
        chooser = std::make_unique<FileChooser>(prompt, File{}, wildcard);
        chooser->launchAsync(writing ? FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                                       | FileBrowserComponent::warnAboutOverwriting
                                     : FileBrowserComponent::openMode | (folder ? FileBrowserComponent::canSelectDirectories
                                                                               : FileBrowserComponent::canSelectFiles),
            [action = std::move(action)](const FileChooser& dialog) {
                if (dialog.getResult() != File{}) action(dialog.getResult());
            });
    }

    void showSettings() {
        if (settingsWindow) { settingsWindow->toFront(true); return; }
        auto* selector = new AudioDeviceSelectorComponent(devices, 0, 0, 1, 2, true, false, true, false);
        selector->setSize(570, 440);
        DialogWindow::LaunchOptions options;
        options.content.setOwned(selector);
        options.dialogTitle = "Audio and MIDI devices";
        options.dialogBackgroundColour = Colour(card);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        settingsWindow = options.launchAsync();
    }

    void showEditor(int lane) {
        editor.reset();
        auto* plugin = engine.tracks[static_cast<size_t>(lane)].plugin.get();
        if (!plugin) return;
        auto* view = plugin->hasEditor() ? plugin->createEditorIfNeeded() : nullptr;
        if (!view) view = new GenericAudioProcessorEditor(*plugin);
        editor = std::make_unique<EditorWindow>(plugin->getName(), view);
    }

    void beginScan(int lane) {
        if (scanner) { report("A VST3 scan is already running."); return; }
        for (auto* format : formats.getFormats()) {
            if (format->getName() != "VST3") continue;
            auto paths = format->getDefaultLocationsToSearch();
            paths.addPath(extraVst3Paths);
            vst3Catalog.clear();
            const auto folder = File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("VOG Studio");
            folder.createDirectory();
            scanner = std::make_unique<PluginDirectoryScanner>(vst3Catalog, *format, paths, true,
                                                               folder.getChildFile("vst3-scan-in-progress.txt"));
            scanTrack = lane;
            report("Scanning standard and custom VST3 folders…");
            return;
        }
        report("This build does not include VST3 hosting.");
    }

    void advanceScan() {
        String name;
        if (scanner->scanNextFile(true, name)) {
            if (name.isNotEmpty()) report("Scanning VST3 · " + name);
            return;
        }
        const auto issues = scanner->getFailedFiles().size() + vst3Catalog.getBlacklistedFiles().size();
        scanner.reset();
        const auto count = vst3Catalog.getNumTypes();
        report(String(count) + " VST3 entries found. " + String(issues) + " skipped or failed.");
        if (count > 0 && scanTrack < static_cast<int>(rows.size())) showVst3Catalog(scanTrack);
    }

    void showVst3Catalog(int lane) {
        if (vst3Catalog.getNumTypes() == 0) { beginScan(lane); return; }
        auto descriptions = vst3Catalog.getTypes();
        std::sort(descriptions.begin(), descriptions.end(), [](const auto& a, const auto& b) {
            return std::tie(a.manufacturerName, a.name) < std::tie(b.manufacturerName, b.name);
        });
        PopupMenu menu, manufacturerMenu;
        String manufacturer;
        for (const auto& description : descriptions) {
            const auto name = description.manufacturerName.isEmpty() ? String("Other") : description.manufacturerName;
            if (name != manufacturer) {
                if (manufacturer.isNotEmpty()) menu.addSubMenu(manufacturer, manufacturerMenu);
                manufacturerMenu.clear();
                manufacturer = name;
            }
            manufacturerMenu.addItem(description.name + (description.isInstrument ? " · Instrument" : " · Effect"),
                                     [this, lane, description] { loadPlugin(lane, description); });
        }
        if (manufacturer.isNotEmpty()) menu.addSubMenu(manufacturer, manufacturerMenu);
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(rows[static_cast<size_t>(lane)].load));
    }

    void pluginMenu(int lane) {
        PopupMenu menu;
        menu.addItem("Browse scanned VST3 plug-ins…", [this, lane] { showVst3Catalog(lane); });
        menu.addItem("Scan standard VST3 folders again", [this, lane] { beginScan(lane); });
        menu.addItem("Add custom VST3 folder and scan…", [this, lane] {
            choose("Select a folder containing VST3 plug-ins", "*", false, [this, lane](File folder) {
                extraVst3Paths.addIfNotAlreadyThere(folder);
                beginScan(lane);
            }, true);
        });
        menu.addSeparator();
        menu.addItem("Import WAV clip…", [this, lane] {
            choose("Choose a WAV clip", "*.wav", false, [this, lane](File file) {
                auto clip = readClip(file);
                if (!clip) { report("Choose a readable mono or stereo WAV up to 60 seconds."); return; }
                {
                    const ScopedLock guard(engine.lock);
                    auto& track = engine.tracks[static_cast<size_t>(lane)];
                    if (track.plugin && (track.description.isInstrument || track.plugin->getTotalNumInputChannels() == 0)) {
                        report("Remove the instrument plug-in before adding a WAV clip.");
                        return;
                    }
                    track.clip = std::move(clip);
                    engine.stop();
                }
                refresh();
                report("Imported " + file.getFileName() + ". Clips start at step 1.");
            });
        });
        menu.addItem("Remove WAV clip", [this, lane] {
            { const ScopedLock guard(engine.lock); engine.tracks[static_cast<size_t>(lane)].clip.reset(); engine.stop(); }
            refresh();
        });
        menu.addItem("Load VST3…", [this, lane] {
            choose("Choose a VST3 file or bundle", "*.vst3", false,
                [this, lane](File file) { scanVst3(lane, file); });
        });
       #if JUCE_WINDOWS
        menu.addItem("Load VST3 folder bundle…", [this, lane] {
            choose("Select the .vst3 folder itself", "*", false,
                [this, lane](File file) { scanVst3(lane, file); }, true);
        });
       #endif
       #if JUCE_MAC
        menu.addItem("Browse installed Audio Units…", [this, lane] {
            OwnedArray<PluginDescription> found;
            for (auto* format : formats.getFormats())
                if (format->getName() == "AudioUnit")
                    for (const auto& id : format->searchPathsForPlugins({}, false, false))
                        format->findAllTypesForFile(found, id);
            chooseDescription(lane, found);
        });
       #endif
        menu.addItem("Remove plug-in", [this, lane] {
            editor.reset();
            {
                const ScopedLock guard(engine.lock);
                auto& track = engine.tracks[static_cast<size_t>(lane)];
                if (track.plugin) track.plugin->releaseResources();
                track.plugin.reset();
                track.description = {};
                engine.stop();
            }
            refresh();
        });
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(rows[static_cast<size_t>(lane)].load));
    }

    void scanVst3(int lane, const File& file) {
        if (!file.hasFileExtension("vst3")) { report("Choose the outer .vst3 bundle."); return; }
        OwnedArray<PluginDescription> found;
        for (auto* format : formats.getFormats())
            if (format->getName() == "VST3") format->findAllTypesForFile(found, file.getFullPathName());
        chooseDescription(lane, found);
    }

    std::unique_ptr<Clip> readClip(const File& file) {
        if (!file.hasFileExtension("wav")) return {};
        std::unique_ptr<AudioFormatReader> reader(audioFormats.createReaderFor(file));
        if (!reader || reader->numChannels == 0 || reader->numChannels > 2
            || reader->sampleRate <= 0 || reader->sampleRate > 192000
            || reader->lengthInSamples <= 0 || reader->lengthInSamples > reader->sampleRate * 60)
            return {};
        auto clip = std::make_unique<Clip>();
        clip->file = file;
        clip->rate = reader->sampleRate;
        const auto count = static_cast<int>(reader->lengthInSamples);
        clip->audio.setSize(static_cast<int>(reader->numChannels), count);
        if (!reader->read(&clip->audio, 0, count, 0, true, true)) return {};
        return clip;
    }

    void chooseDescription(int lane, const OwnedArray<PluginDescription>& found) {
        if (found.isEmpty()) { report("No supported plug-ins were found at that location."); return; }
        if (found.size() == 1) { loadPlugin(lane, *found[0]); return; }
        PopupMenu menu;
        for (auto* entry : found)
            menu.addItem(entry->name, [this, lane, description = *entry] { loadPlugin(lane, description); });
        menu.showMenuAsync(PopupMenu::Options().withTargetComponent(rows[static_cast<size_t>(lane)].load));
    }

    void loadPlugin(int lane, const PluginDescription& description) {
        if (lane < 0 || lane >= static_cast<int>(rows.size())) return;
        String error;
        double rate;
        { const ScopedLock guard(engine.lock); rate = engine.rate; }
        auto instance = formats.createPluginInstance(description, rate, block, error);
        if (!instance) { report("Plug-in could not load. " + error); return; }
        if (engine.tracks[static_cast<size_t>(lane)].clip
            && (description.isInstrument || instance->getTotalNumInputChannels() == 0)) {
            report("Remove the WAV clip before loading an instrument plug-in.");
            return;
        }
        editor.reset();
        {
            const ScopedLock guard(engine.lock);
            auto& track = engine.tracks[static_cast<size_t>(lane)];
            if (track.plugin) track.plugin->releaseResources();
            engine.prepare(*instance);
            track.plugin = std::move(instance);
            track.description = description;
            engine.stop();
        }
        refresh();
        report("Loaded " + description.name);
    }

    void saveProject() {
        choose("Save VOG Studio project", "*.vogstudio", true, [this](File file) {
            XmlElement project("VOGStudio");
            project.setAttribute("version", 1);
            {
                const ScopedLock guard(engine.lock);
                project.setAttribute("tempo", engine.bpm);
                for (const auto& track : engine.tracks) {
                    auto* item = project.createNewChildElement("track");
                    String pattern;
                    for (const auto active : track.pattern) pattern += active ? "1" : "0";
                    item->setAttribute("steps", pattern);
                    item->setAttribute("note", track.note);
                    item->setAttribute("gain", track.gain);
                    item->setAttribute("mute", track.mute ? 1 : 0);
                    if (track.clip)
                        item->setAttribute("clip", track.clip->file.getFullPathName());
                    if (track.plugin) {
                        auto description = track.description.createXml();
                        item->addChildElement(description.release());
                        MemoryBlock state;
                        track.plugin->getStateInformation(state);
                        item->setAttribute("state", state.toBase64Encoding());
                    }
                }
            }
            report(project.writeTo(file.withFileExtension(".vogstudio")) ? "Project saved." : "Project save failed.");
        });
    }

    void openProject() {
        choose("Open VOG Studio project", "*.vogstudio", false, [this](File file) {
            auto project = XmlDocument::parse(file);
            if (!project || !project->hasTagName("VOGStudio") || project->getIntAttribute("version") != 1
                || project->getNumChildElements() == 0) {
                report("This file is not a supported VOG Studio project."); return;
            }
            double rate;
            { const ScopedLock guard(engine.lock); rate = engine.rate; }
            std::deque<Track> loaded;
            StringArray failures;
            for (auto* item : project->getChildIterator()) {
                if (!item->hasTagName("track")) { report("Project track list is invalid."); return; }
                auto& track = loaded.emplace_back();
                const auto clipPath = item->getStringAttribute("clip");
                if (clipPath.isNotEmpty()) {
                    if (File::isAbsolutePath(clipPath)) track.clip = readClip(File(clipPath));
                    if (!track.clip) failures.add(clipPath);
                }
                if (auto* xml = item->getChildByName("PLUGIN")) {
                    if (!track.description.loadFromXml(*xml)) { report("Invalid plug-in entry."); return; }
                    String error;
                    track.plugin = formats.createPluginInstance(track.description, rate, block, error);
                    if (!track.plugin || (track.clip
                        && (track.description.isInstrument || track.plugin->getTotalNumInputChannels() == 0))) {
                        failures.add(track.description.name);
                    } else {
                        MemoryBlock state;
                        if (state.fromBase64Encoding(item->getStringAttribute("state")) && state.getSize() > 0)
                            track.plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                    }
                }
                track.note = jlimit(0, 127, item->getIntAttribute("note", 60));
                track.gain = static_cast<float>(jlimit(0.0, 1.0, item->getDoubleAttribute("gain", 0.75)));
                track.mute = item->getBoolAttribute("mute");
                const auto pattern = item->getStringAttribute("steps").paddedRight('0', steps);
                for (int step = 0; step < steps; ++step)
                    track.pattern[static_cast<size_t>(step)] = pattern[step] == '1';
            }
            if (!failures.isEmpty()) {
                report("Project kept closed. Restore these plug-ins or WAV files first: " + failures.joinIntoString(", "));
                return;
            }
            PopupMenu::dismissAllActiveMenus();
            scanner.reset();
            editor.reset();
            {
                const ScopedLock guard(engine.lock);
                for (auto& track : loaded)
                    if (track.plugin) engine.prepare(*track.plugin);
                engine.stop();
                engine.selected = 0;
                engine.bpm = jlimit(5.0, 300.0, project->getDoubleAttribute("tempo", 120.0));
                engine.tracks.swap(loaded);
            }
            for (auto& track : loaded)
                if (track.plugin) track.plugin->releaseResources();
            rows.clear();
            for (int i = 0; i < project->getNumChildElements(); ++i) setupRow(i);
            resized();
            trackViewport.setViewPosition(0, 0);
            refresh();
            report("Project opened.");
        });
    }

    void exportMidi() {
        if (rows.size() >= 65535) {
            report("Standard MIDI export supports at most 65,534 instrument tracks.");
            return;
        }
        choose("Export standard MIDI file", "*.mid", true, [this](File file) {
            MidiFile result;
            result.setTicksPerQuarterNote(480);
            {
                const ScopedLock guard(engine.lock);
                MidiMessageSequence conductor;
                conductor.addEvent(MidiMessage::tempoMetaEvent(roundToInt(60000000.0 / engine.bpm)));
                conductor.addEvent(MidiMessage::timeSignatureMetaEvent(4, 4));
                result.addTrack(conductor);
                for (const auto& track : engine.tracks) {
                    MidiMessageSequence sequence;
                    if (!track.mute && !track.clip)
                        for (int i = 0; i < steps; ++i)
                            if (track.pattern[static_cast<size_t>(i)]) {
                                sequence.addEvent(MidiMessage::noteOn(1, track.note, uint8(100)), i * 120.0);
                                sequence.addEvent(MidiMessage::noteOff(1, track.note), i * 120.0 + 86.0);
                            }
                    sequence.updateMatchedPairs();
                    result.addTrack(sequence);
                }
            }
            TemporaryFile temp(file.withFileExtension(".mid"));
            auto stream = temp.getFile().createOutputStream();
            const bool written = stream && result.writeTo(*stream, 1);
            stream.reset();
            report(written && temp.overwriteTargetFileWithTemporary() ? "MIDI exported." : "MIDI export failed.");
        });
    }
};

class MainWindow final : public DocumentWindow {
public:
    MainWindow() : DocumentWindow("VOG Studio", Colour(navy), allButtons) {
        setUsingNativeTitleBar(true);
        setContentOwned(new Studio, true);
        setResizable(true, false);
        setResizeLimits(1050, 455, 2200, 1000);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class App final : public JUCEApplication {
public:
    const String getApplicationName() override { return "VOG Studio"; }
    const String getApplicationVersion() override { return "0.2.0"; }
    void initialise(const String&) override { window = std::make_unique<MainWindow>(); }
    void shutdown() override { window.reset(); }
private:
    std::unique_ptr<MainWindow> window;
};
}
START_JUCE_APPLICATION(App)
