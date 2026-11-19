# VOG Studio 0.2.0

VOG Studio is a desktop DAW starter for Windows x64 and macOS. Its workspace is a one-bar loop sequencer that starts with four tracks. Add as many tracks as your computer can reliably play. This package contains C++ source and a GitHub Actions workflow for building the application.

## Build

1. Extract this ZIP and put the **contents** of the `VOG_Studio` folder at the root of your GitHub repository. Include the hidden `.github` folder.
2. Open **Actions → Build VOG Studio → Run workflow**, or push a commit with the workflow included.
3. In the completed run, download **VOG_Studio_Windows_x64** or **VOG_Studio_macOS** under **Artifacts**. On Windows, extract and run **VOG Studio.exe**. On macOS, extract the inner ZIP and open **VOG Studio.app**. These builds are unsigned.

For a local build, install Git, CMake 3.22 or newer, and the Microsoft C++ desktop build tools on Windows or Xcode on macOS. CMake downloads the pinned JUCE 8.0.8 source.

Windows, from PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

macOS, from Terminal:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build --config Release
```

The app is under `build/VOGStudio_artefacts/Release/`. Review the [JUCE licensing terms](https://juce.com/get-juce/) before distributing builds.

## Make a loop

1. Open **Audio / MIDI** and select your output and MIDI keyboard. Select a track to route live MIDI from a keyboard such as the MiniNova.
2. Click **Add Track** whenever you need another track. The track area scrolls vertically. Toggle steps and press **Play**. Each track has a pitch, level slider, and mute control. The built-in tone can play a pattern before you install plug-ins.
3. Click **Scan VST3** to search the OS standard folders recursively. On Windows that includes `C:\Program Files\Common Files\VST3` and the current user's local VST3 folder. On macOS that includes both system and user `Library/Audio/Plug-Ins/VST3` folders. Pick a plug-in from the manufacturer menu to load it on the selected track. From **Source / FX** you can browse the same results on any track, rescan after installing a plug-in, or add a custom search folder such as a plug-in folder on your E drive. Custom folders and scan results are kept for the current app session.
4. **Source / FX** also supports direct loading and mono/stereo WAV import. On Windows a directory-format VST3 uses **Load VST3 folder bundle**. On macOS you can also browse installed Audio Units that support synchronous instantiation. Click the plug-in name to open its editor.
5. Use one instrument plug-in per track, or put one input effect on a WAV or internal-tone track. WAV clips start at step 1 and play at their original speed for up to one bar. Use the level controls to balance tracks before exporting.
6. Save the session as a `.vogstudio` file. It stores patterns, tempo, levels, mute and plug-in state and references installed plug-ins and WAV files by location. Keep the original plug-ins and WAVs available to reopen the session. Earlier four-track projects also open in this version.
7. **Export WAV** renders one stereo 24-bit bar at the current device sample rate. **Export MIDI** writes a type-1 standard MIDI file with tempo, meter and the active note tracks. Use WAV for the sound of your plug-ins and MIDI for editable notes.

## Reason 14 and Logic Pro

| Workflow | How to use it |
| --- | --- |
| Reason Rack inside VOG Studio | Install and authorize Reason 14, then load the Reason Rack Plugin VST3 as an instrument or effect. The Reason Rack plug-in is supplied by Reason Studios. |
| VOG Studio to Reason 14 | Import the WAV bounce on an audio track or import the standard MIDI file and assign instruments in Reason. Compatible VST3s can be installed in both hosts. |
| VOG Studio to Logic Pro on Mac | Import the WAV or MIDI export. For the same virtual instrument or effect, install the vendor’s Audio Unit edition and select it in Logic. Logic uses Audio Units for third-party plug-ins. |

VOG Studio runs as a standalone app. Native Reason song and Logic project import, cross-host plug-in preset conversion, plug-in wrapping, multibar arrangement, and audio recording are outside this version. The WAV export is a one-bar mix with no extra tail. The host loads plug-ins in-process. Plug-ins must match the system architecture and vendor authorization requirements.

[Reason Rack Plugin 14 manual](https://docs.reasonstudios.com/rackplugin14/introduction) · [Audio Units in Logic Pro](https://support.apple.com/en-am/guide/logicpro/lgcp22a0dab0/mac)

## Verification

The source was syntax-checked against JUCE 8.0.8. Windows and macOS binaries, audio device playback, and third-party plug-in loading still require platform testing. After building, first play the built-in tone, then load Reason Rack, save and reopen a project, and import the WAV and MIDI exports into your target DAW.
