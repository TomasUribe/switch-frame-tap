# Building the Android TV viewer

The viewer is a standalone Gradle project in `android-tv/`. These instructions
build the Android receiver; the Switch sysmodule has its own build environment.

## Requirements

| Component | Version or purpose |
|---|---|
| Operating system | Windows, Linux, or macOS |
| Java Development Kit | **JDK 17**, including `java` and `javac` |
| Android SDK Platform | **Android 14 / API 34** |
| Android SDK Build Tools | **34.0.0** |
| Android SDK Command-line Tools | SDK installation and license management outside Android Studio |
| Gradle | **8.5**, downloaded by the included wrapper |
| Android Gradle Plugin | **8.3.0**, downloaded by Gradle |
| Python | Optional; Python 3.10+ for packaging and replay utilities |
| Android SDK Platform Tools | Optional; provides `adb` for installation and debugging |

Android Studio is optional. The project uses Java and Android platform APIs, with
no additional Python packages needed for its optional utilities. The first build
requires internet access to download Gradle and build dependencies.

The JDK and Build Tools versions follow the
[Android Gradle Plugin 8.3 compatibility requirements](https://developer.android.com/build/releases/agp-8-3-0-release-notes).

## 1. Check out the source

Check out the branch containing the Android viewer. From the repository root:

```shell
cd android-tv
```

Run subsequent commands from this directory unless stated otherwise.

## 2. Configure Java and the SDK

Set `JAVA_HOME` to the JDK 17 installation directory and `ANDROID_HOME` to the Android
SDK directory. Replace the example paths with your actual installations.

Windows PowerShell:

```powershell
$env:JAVA_HOME = "C:\path\to\jdk-17"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
$env:Path = "$env:JAVA_HOME\bin;$env:ANDROID_HOME\cmdline-tools\latest\bin;$env:ANDROID_HOME\platform-tools;$env:Path"
```

Linux/macOS:

```bash
export JAVA_HOME="/path/to/jdk-17"
export ANDROID_HOME="/path/to/android-sdk"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"
```

These commands apply to the current terminal session. `JAVA_HOME` must point to the
JDK root, not its `bin` directory. The command-line tools examples assume the SDK's
`cmdline-tools/latest/` layout.

Alternatively, specify the SDK path in `android-tv/local.properties`:

```properties
sdk.dir=C:/Users/YOUR_USER/AppData/Local/Android/Sdk
```

Use your actual SDK location. This file is machine-specific and excluded from Git.
When using Android Studio, open `android-tv/` and select JDK 17 as its Gradle JDK.

## 3. Install SDK packages

Use Android Studio's SDK Manager to install Platform 34, Build Tools 34.0.0, and
optionally Platform Tools. With the Command-line Tools installed and on `PATH`,
the equivalent commands are:

```shell
sdkmanager "platforms;android-34" "build-tools;34.0.0" "platform-tools"
sdkmanager --licenses
```

Review and accept the required licenses. See the
[SDK Manager documentation](https://developer.android.com/tools/sdkmanager).

## 4. Build a debug APK

Windows PowerShell:

```powershell
.\gradlew.bat :app:assembleDebug
```

Linux/macOS:

```bash
bash gradlew :app:assembleDebug
```

The wrapper selects Gradle 8.5 automatically. No separate Gradle installation is
needed. The output, relative to the repository root, is:

```text
android-tv/app/build/outputs/apk/debug/app-debug.apk
```

The debug APK is signed with the development machine's debug key and is intended
for testing. Updating another APK in place requires a matching signing key.

## 5. Run development checks

Windows PowerShell:

```powershell
.\gradlew.bat :app:assembleDebug :app:protocolTest :app:playbackTest :app:lintDebug
```

Linux/macOS:

```bash
bash gradlew :app:assembleDebug :app:protocolTest :app:playbackTest :app:lintDebug
```

- `protocolTest` checks SFTR parsing, validation, fragmented reads, and reference
  recovery. With the full repository, it also reads the capture in
  `logs/m83-runI/stream-first10.sft`. Without the capture, it runs synthetic checks.
- `playbackTest` runs production worker loops against delayed/stalled driver test
  doubles. It uses the real capture when available and synthetic framing otherwise.
  The Android API doubles are compiled separately and are not part of the APK.
- `lintDebug` performs Android static analysis.

These checks run on the host without a Switch, TV, or emulator. They do not replace
physical device tests. See [VALIDATION.md](VALIDATION.md) for the TV test plan.

## 6. Optional distribution packaging

After building:

```shell
python tools/package.py
```

Use `python3` if that is the Python 3 command on your system. The script creates
these files under the repository's `dist/` directory:

```text
SwitchFrameTapTV-0.1.2-preview.apk
SwitchFrameTapTV-0.1.2-preview-source.zip
SwitchFrameTapTV-0.1.2-preview-SHA256.txt
```

The source ZIP excludes build outputs, caches, and `local.properties`.

## 7. Optional ADB installation

With the TV connected and authorized through ADB:

```shell
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

To collect application errors:

```shell
adb logcat -s SFT-TV
```

## Runtime requirements

The viewer requires Android TV / Google TV with **Android 6.0 / API 23 or later**.
Playback requires Switch Frame Tap running on the console and either a shared
local network or compatible USB host access on the Android device. The controller
remains connected to the Switch. No console or TV is needed to compile this project.