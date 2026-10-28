# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Wunderfitz is a SailfishOS mobile dictionary application (Qt/QML + C++) supporting:
- The bundled Heinzelnisse Norwegian-German dictionary (SQLite database at `db/heinzelliste.db`)
- User-imported dict.cc ZIP export files
- Optional Azure Cognitive Services OCR and translation (Camera → text → translate)

## Build System

This is a qmake project targeting SailfishOS. Building requires the Sailfish SDK (SFDK / Platform SDK). There are no unit tests.

**Project structure** (two subprojects, in dependency order):
1. `quazip/quazip/wunderfitz-quazip.pro` — builds `libquazip.so` (installed to `/usr/share/harbour-wunderfitz/lib`)
2. `src/src.pro` — builds the main app binary, depends on quazip

**Building with Sailfish SDK:**
```bash
/c/SailfishOS/bin/sfdk.exe build
```
(`sfdk` is not on the default PATH in the bash shell; use the full path above.)

**Building manually (inside the Platform SDK/build engine):**
```bash
qmake harbour-wunderfitz.pro
make
```

**Updating translations** (after adding new `tr()` strings):
```bash
lupdate harbour-wunderfitz.pro
```

**Packaging as RPM:**
```bash
/c/SailfishOS/bin/sfdk.exe package
```

The RPM spec is at `rpm/harbour-wunderfitz.spec`.

**Deploying to device:** `sfdk deploy` requires the device to be registered via the Sailfish IDE — it is not configured in the CLI by default. Deploy built RPMs through the Sailfish IDE (Qt Creator: Deploy button) or by copying the `.rpm` to the device and installing via the file manager. The built RPM lands in `RPMS/` after `sfdk build`.

## Architecture

### C++ Backend (`src/`)

All C++ objects are exposed to QML via `QQmlContext::setContextProperty()` in `harbour-wunderfitz.cpp`:
- `dictionaryModel` — `DictionaryModel`
- `heinzelnisseModel` — `DictionaryModel::heinzelnisseModel` (a `HeinzelnisseModel`)
- `dictCCImporterModel` — `DictionaryModel::dictCCImporterModel` (a `DictCCImporterModel`)
- `curiosity` — `Curiosity`
- `cloudApi` — `Curiosity::cloudApi` (a `CloudApi`)

**Key classes and responsibilities:**

| Class | Role |
|---|---|
| `DictionaryModel` | Top-level `QAbstractListModel` of available dictionaries; owns `HeinzelnisseModel` and `DictCCImporterModel`; manages selected dictionary |
| `HeinzelnisseModel` | `QAbstractListModel` of search results; delegates to `DatabaseManager` for SQLite queries |
| `DatabaseManager` | Manages the active SQLite connection; spawns `DictionarySearchWorker` thread per search |
| `DictionarySearchWorker` | `QThread` — runs the actual SQL search; emits `searchCompleted` signal |
| `DictCCImporterModel` | `QAbstractListModel` for import status/results; spawns `DictCCImportWorker` |
| `DictCCImportWorker` | `QThread` — scans for dict.cc ZIP files (via QuaZIP), parses them, writes to SQLite |
| `Curiosity` | Manages camera capture workflow, settings persistence, and wires `CloudApi` signals to QML-facing signals |
| `CloudApi` | Azure Computer Vision (OCR) and Translator Text REST API calls via `QNetworkAccessManager` |

**Search flow:** QML calls `heinzelnisseModel.search(query)` → `HeinzelnisseModel` → `DatabaseManager::updateResults()` → `DictionarySearchWorker` (background thread) → emits `searchCompleted` → model resets results.

**Import flow:** QML calls `dictCCImporterModel.importDictionaries()` → `DictCCImportWorker` (background thread) reads ZIP files from standard download locations → writes SQLite DBs → emits `importFinished`.

**OCR/translate flow:** QML calls `curiosity.captureRequested()` → `curiosity.captureCompleted(path)` → `cloudApi.opticalCharacterRecognition()` → on success, `cloudApi.translate()` → `curiosity` emits `translationSuccessful`.

### QML Frontend (`qml/`)

Uses the Sailfish Silica UI framework. Entry point: `qml/harbour-wunderfitz.qml` (ApplicationWindow).

**Pages:**
- `TitlePage.qml` — main page with search field and results list; contains tab navigation (Dictionaries / Camera / Result tabs)
- `DictionariesPage.qml` — manage/select active dictionary
- `SettingsPage.qml` — Azure API keys, endpoints, source/target language settings
- `TextPage.qml` — displays OCR/translation result text
- `AboutPage.qml`, `CoverPage.qml` — standard SailfishOS pages

**Reusable components** (`qml/components/`): `WunderfitzButton`, `DictionaryButton`, `LoadingIndicator`, `AppNotification`, `AppNotificationItem`.

### Settings

Settings keys and Azure endpoint defaults are defined as `const char[]` in `src/curiosity.h`. Settings are stored via `QSettings` (SailfishOS per-app settings).

**Azure API keys are stored in Sailfish Secrets** (not QSettings). The collection is `"wunderfitz"` (the SQLCipher plugin only accepts alphanumeric collection names) using `DefaultEncryptedStoragePluginName` for both storage and encryption plugin with `DeviceLockKeepUnlocked` — unlocked transparently whenever the device screen lock is satisfied, no extra user prompt. On first read, a migration path checks QSettings for a legacy value, moves it to Secrets, and removes it from QSettings.

**Linking `libsailfishsecrets` in `src.pro`:** Use `LIBS += -lsailfishsecrets` directly. Do **not** use `CONFIG += link_pkgconfig` + `PKGCONFIG += sailfishsecrets` — this breaks the `sailfishapp` library linking because it interferes with qmake feature-loading order (`sailfishapp.prf` already manages `link_pkgconfig` internally).

**Sailfish Secrets header includes:** The pkgconfig `includedir` is already `/usr/include/Sailfish/`, so includes use the `<Secrets/...>` prefix with lowercase `.h` filenames:
```cpp
#include <Secrets/secretmanager.h>
#include <Secrets/secret.h>
#include <Secrets/result.h>
#include <Secrets/createcollectionrequest.h>
#include <Secrets/storesecretrequest.h>
#include <Secrets/storedsecretrequest.h>
```
Not `<Sailfish/Secrets/SecretManager>` (Qt module-style headers do not exist for this library).

**RPM packaging:** Add `BuildRequires: pkgconfig(sailfishsecrets)` to the spec. Do **not** add `Requires: sailfish-secrets` — it is a system component not available in user-accessible package repos, causing RPM install to fail with an unsatisfiable dependency even though the library is already on every device.

### Translations

Translation files: `translations/harbour-wunderfitz-<lang>.ts`. The base file `translations/harbour-wunderfitz.ts` is the source template. Add translations by editing the relevant `.ts` file and running `lrelease`.

**`type="unfinished"` entries are skipped:** The Sailfish SDK's `lrelease` runs with `-nounfinished`, so any `<translation type="unfinished">` entry is **not compiled into the `.qm` file** and falls back to English at runtime. When `lupdate` encounters a string that exists in multiple QML files it adds duplicate entries marked `type="unfinished"` in secondary contexts. Always remove the `type="unfinished"` attribute (keep the translation text) before building.

**Translation context is per QML file:** `qsTr("Foo")` in `TitlePage.qml` looks up context `TitlePage`; the same literal in `TextPage.qml` looks up context `TextPage`. Each context needs its own translation entry in the `.ts` file. If the same string appears in both files, both context blocks need a finished translation.

## SailfishOS QML Patterns

### Selectable text inside SilicaFlickable

To display text that users can select (long-press → drag handles → copy), use `TextArea` **without** `readOnly: true`. Use a `SectionHeader` placed **above** the `TextArea` for the label (the `label` property on `TextArea` renders *below* the content, not above):

```qml
SilicaFlickable {
    contentHeight: column.height
    anchors.fill: parent

    Column {
        id: column
        width: parent.width

        SectionHeader {
            text: qsTr("My label")
        }

        TextArea {
            width: parent.width
            wrapMode: TextEdit.Wrap
            text: someString
        }
    }

    VerticalScrollDecorator {}
}
```

**Why `readOnly: true` must not be used:** Silica's `TextArea` is built on an internal `TextBase` component that manages its own `MouseArea`. This MouseArea sets `preventStealing = true` when a selection handle is grabbed, which is what allows selection to coexist with `SilicaFlickable` scrolling. Setting `readOnly: true` disables this entire interaction layer — the result is a completely unresponsive component with no selection, no cursor, and no handles.

**`ContextMenu` on `ListItem` is not a substitute** for text selection — it only allows copying the full string, not a highlighted portion.

**`TextArea` `label` property renders below content:** The `label:` property in Silica `TextArea` is displayed *underneath* the text content, not above it as a heading. Always use a `SectionHeader` above the `TextArea` when you want a visible section title at the top.

### VerticalScrollDecorator placement

`VerticalScrollDecorator` must be a direct child of `SilicaFlickable`, not nested inside a `Column` or other layout child.

### SlideshowView tab navigation vs. page-stack forward navigation

`SlideshowView` (used in `TitlePage.qml` for the Dictionaries / Camera / Result tabs) intercepts **all** horizontal swipe gestures across its full area. This means `pageStack.pushAttached()` + forward swipe is **not usable** when the current page contains a `SlideshowView` — the slideshow always wins the gesture.

The correct pattern for swipe-accessible content on a page that already uses `SlideshowView` is to add a new tab to the slideshow itself, not to use the page stack:

```qml
// In viewsModel (VisualItemModel):
Item {
    id: myNewTab
    width: viewsSlideshow.width
    height: viewsSlideshow.height
    // ... content ...
}

// Switch to it programmatically:
viewsSlideshow.opacity = 0;
slideshowVisibleTimer.goToTab(2);
openTab(2);
```

Each tab needs a corresponding navigation button in both the portrait bottom bar (`navigationRow`) and the landscape side bar (`navigationColumn`). When adding a tab, update all `width: parent.width / N` and `height: parent.height / N` values to reflect the new count.

### PullDownMenu in nested SilicaFlickable

When a page has an outer `SilicaFlickable` that wraps a `SlideshowView` (or any other content), and individual tabs also contain inner `SilicaFlickable` elements with their own `PullDownMenu`, **only the outermost `PullDownMenu` is reachable** — the outer flickable captures the pull gesture first. Inner `PullDownMenu` items are dead code and will never fire.

The correct pattern is to put **all** menu items in the single outer `PullDownMenu`, using conditional `visible:` to show tab-specific items only when the relevant tab is active:

```qml
// Outer SilicaFlickable's PullDownMenu — the only one the user can reach
PullDownMenu {
    MenuItem {
        text: qsTr("About")
        onClicked: pageStack.push(aboutPage)
    }
    // Tab-specific items: only visible on the relevant tab
    MenuItem {
        visible: titlePage.activeTabId === 0
        text: qsTr("Dictionaries")
        onClicked: pageStack.push(dictionariesPage)
    }
    MenuItem {
        visible: titlePage.activeTabId === 2
        text: qsTr("Copy translation to clipboard")
        onClicked: Clipboard.text = titlePage.lastTranslation
    }
}
```

Do not add a `PullDownMenu` inside an inner `SilicaFlickable` that lives inside a `SlideshowView` — it will never be triggered.

### Sailfish.WebView import causes white screen on launch

Adding `import Sailfish.WebView 1.0` and `import Sailfish.WebEngine 1.0` to any QML page causes the entire app to show a completely white screen on launch — no error, no crash, just a blank display. The root cause is likely that the WebEngine requires explicit initialization before any `WebView` element is instantiated.

**Do not add these imports** until the initialization issue is understood. The `sailfish-components-webview-qt5` package must also be listed as a `Requires:` in `rpm/harbour-wunderfitz.spec` when WebView is used.

When revisiting: investigate `WebEngineSettings` / WebEngine initialization calls that may be needed before the first `WebView` is shown.

### Conditionally hiding a SlideshowView tab button

To show a tab navigation button only when content is available, add a boolean property on the page and gate both the button visibility and the swipe handler:

```qml
// On the Page:
property bool hasResult: lastOriginal !== "" || lastTranslation !== ""

// Navigation button (portrait and landscape):
Item {
    id: resultButtonColumn
    visible: titlePage.hasResult
    // ...
}

// SlideshowView — block swipe to the tab when content is absent:
SlideshowView {
    onCurrentIndexChanged: {
        if (currentIndex === 2 && !titlePage.hasResult) {
            slideshowVisibleTimer.goToTab(1);
            openTab(1);
        } else {
            openTab(currentIndex);
        }
    }
}
```

In a `Row` layout, `visible: false` removes the item from the layout (no gap left behind).

### TextArea bindings across tab switches

When a `TextArea` is shared across tab navigation (i.e., its `text` is bound to a property on a parent item), user interaction can break the QML binding. Use an explicit `Connections` block to force re-sync whenever the source property changes:

```qml
TextArea {
    id: myTextArea
    text: titlePage.lastOriginal
    Connections {
        target: titlePage
        onLastOriginalChanged: myTextArea.text = titlePage.lastOriginal
    }
}
```
