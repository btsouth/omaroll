import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

// Settings exist to change defaults, never to make the app work. Everything
// here has a working value on a fresh install, which is why this window is
// small and why nothing in it is required reading.
Item {
    id: root

    signal rescanRequested()
    signal createAlbumRequested()
    property string folderMessage: ""
    property bool pinSelectedFolder: false
    property string textCacheMessage: ""
    property string organizationMessage: ""
    // What the last default-viewer change did, in a sentence.
    property string defaultsMessage: ""
    property bool defaultsOk: true
    // What the last window-rules change did, and the state it left.
    property string windowRulesMessage: ""
    property string windowRulesState: "unsupported"
    // Optional programs that aren't installed, read each time Settings opens.
    property var missingTools: []
    property bool organizationOk: false
    // Set once a backup file has been chosen; the restore runs only after the
    // user confirms, since it replaces the current organization.
    property string pendingRestorePath: ""
    // Asked before the last good backup replaces a damaged file's organization.
    property bool confirmingBackupRestore: false
    // The result, shown where the restore button was.
    property string backupRestoreMessage: ""
    property bool backupRestoreOk: false

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // The sections, in order, for the side index.
    readonly property var sectionItems: [librarySection, viewerSection, videoSection, searchSection,
                                         albumsSection, integrationSection, aboutSection]
    // The section asked for from the index, until the list is next moved by
    // hand: the last few cannot scroll to the top, so position alone would
    // mark the final one.
    property int requestedSection: -1
    readonly property int currentSection: {
        if (root.requestedSection >= 0) return root.requestedSection
        const top = settingsFlickable.contentY
        const maximum = settingsFlickable.contentHeight - settingsFlickable.height
        if (maximum > 0 && top >= maximum - 1) return root.sectionItems.length - 1
        let current = 0
        for (let i = 0; i < root.sectionItems.length; ++i) {
            if (root.sectionItems[i].y + column.y <= top + 48) current = i
        }
        return current
    }

    function showSection(index) {
        const section = root.sectionItems[index]
        if (!section) return
        const maximum = Math.max(0, settingsFlickable.contentHeight - settingsFlickable.height)
        const target = Math.max(0, section.y + column.y - 12)
        // Within a little of the end, go to the end rather than leave a
        // sliver of padding below the last section, if its title stays shown.
        settingsFlickable.contentY = maximum - target < 48 && section.y + column.y >= maximum
                                     ? maximum : Math.min(maximum, target)
        root.requestedSection = index
    }

    function applyPendingRestore() {
        if (root.pendingRestorePath === "") {
            return
        }
        const result = Settings.importOrganization(root.pendingRestorePath)
        root.pendingRestorePath = ""
        root.organizationOk = result.ok
        root.organizationMessage = result.message
        if (result.ok) {
            Captures.setAlbumFilter("", [])
            Captures.setTagFilter("", [])
            Captures.clearSmartCollection()
        }
    }

    // FileDialog hands back a file:// url; the C++ side wants a plain path.
    function localPath(url) {
        const text = String(url)
        return text.indexOf("file://") === 0 ? decodeURIComponent(text.substring(7)) : text
    }

    function nextValue(values, current) {
        return values[(values.indexOf(current) + 1) % values.length]
    }

    function actionLabel(action) {
        const labels = { "preview": "View in Omaroll", "omaframe": "Open in Omaframe",
                         "corrections": "Crop, rotate, resize", "matte": "Add background", "view": "View full size",
                         "edit": "Edit in Pinta", "trim": "Trim", "play": "Play" }
        return labels[action] + (action !== "preview" && !Registry.available(action)
                                ? " (not installed)" : "")
    }

    function availableDefaults(values) {
        return values.filter(function(id) { return id === "preview" || Registry.available(id) })
    }

    function cacheLabel(megabytes) {
        return megabytes === 1024 ? "1 GB" : megabytes + " MB"
    }

    function open() {
        folderMessage = ""
        textCacheMessage = ""
        organizationMessage = ""
        organizationOk = false
        pendingRestorePath = ""
        confirmingBackupRestore = false
        backupRestoreMessage = ""
        defaultsMessage = ""
        windowRulesMessage = ""
        windowRulesState = Settings.windowRules
        missingTools = Settings.missingTools()
        MimeDefaults.refresh()
        requestedSection = -1
        settingsFlickable.contentY = 0
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

    function defaultsStatus() {
        if (root.defaultsMessage !== "") return root.defaultsMessage
        if (!MimeDefaults.known) return "Checking which app opens pictures and videos…"
        if (MimeDefaults.imagesDefault && MimeDefaults.videosDefault)
            return "Pictures and videos open with Omaroll from the file manager."
        if (MimeDefaults.imagesDefault) return "Pictures open with Omaroll. Videos open with another app."
        if (MimeDefaults.videosDefault) return "Videos open with Omaroll. Pictures open with another app."
        return "Pictures and videos open with other apps. Folders are never changed."
    }

    Connections {
        target: MimeDefaults
        function onFinished(ok, summary) {
            root.defaultsOk = ok
            root.defaultsMessage = summary
        }
    }

    visible: false
    anchors.fill: parent
    focus: visible

    // Own wheel input for the whole modal. A Flickable stops accepting wheel
    // events at its bounds, which otherwise lets the same event reach the
    // library underneath and scroll it while Settings is open.
    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            // Pixels only for a touchpad's phased swipe: Wayland gives a
            // wheel notch a pixel delta of a few pixels as well.
            const delta = event.phase !== Qt.NoScrollPhase && event.pixelDelta.y !== 0
                          ? event.pixelDelta.y : event.angleDelta.y / 2
            const minimum = settingsFlickable.originY
            const maximum = minimum + Math.max(
                                0, settingsFlickable.contentHeight - settingsFlickable.height)
            settingsFlickable.contentY = Math.max(
                        minimum, Math.min(maximum, settingsFlickable.contentY - delta))
            root.requestedSection = -1
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)
        // A MouseArea rather than a TapHandler: a TapHandler only takes a
        // passive grab, so a tap on a control inside the card also arrived
        // here and closed the sheet. Every button is accepted so a right
        // click cannot fall through to a tile behind the scrim.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
            onClicked: function (mouse) {
                if (mouse.button === Qt.LeftButton) {
                    root.close()
                }
            }
        }
    }

    // A titled group of settings. The side index lists these by title.
    component SettingsSection: Column {
        id: section
        property string title: ""
        width: parent ? parent.width : 0
        spacing: 16

        Column {
            width: parent.width
            spacing: 8
            Text {
                text: section.title
                font.family: Theme.fontFamily
                font.pixelSize: 14
                font.weight: Font.DemiBold
                color: Theme.brightForeground
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Rectangle {
                width: parent.width
                height: 1
                color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.12)
            }
        }
    }

    // A folder or album under the setting that lists it.
    component ListedPath: Column {
        id: listed
        property string label: ""
        property string path: ""
        property bool available: true
        width: parent ? parent.width : 0
        spacing: 1
        Text {
            width: parent.width
            visible: listed.label !== ""
            text: listed.label
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: Theme.foreground
        }
        Text {
            width: parent.width
            text: listed.path
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: listed.available ? Theme.mutedText : Theme.red
        }
    }

    Rectangle {
        id: card
        objectName: "settingsCard"
        anchors.centerIn: parent
        // Wider and taller on a big window, with a side index once there is
        // room for one; a narrow window gets the sections one after another.
        width: Math.min(root.width - (root.width < 700 ? 24 : 64), 940)
        height: Math.min(root.height - (root.height < 600 ? 24 : 64), 880)
        readonly property bool wide: width >= 720
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        // Clicks inside the card must not reach the scrim behind it.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Column {
            id: header
            x: 22
            y: 18
            width: parent.width - 44
            spacing: 4

            Text {
                text: "Settings"
                font.family: Theme.fontFamily
                font.pixelSize: 16
                font.weight: Font.DemiBold
                color: Theme.brightForeground
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Omaroll reads the folders Omarchy already writes captures to. "
                      + "Rename and deletion happen only when you request them."
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.mutedText
            }
        }

        Rectangle {
            id: headerRule
            anchors.top: header.bottom
            anchors.topMargin: 14
            width: parent.width
            height: 1
            color: root.shade(Theme.foreground, 0.12)
        }

        // The side index: one entry per section, the one in view marked.
        Column {
            id: sectionIndex
            objectName: "settingsIndex"
            visible: card.wide
            anchors.top: headerRule.bottom
            anchors.topMargin: 14
            x: 14
            width: 168
            spacing: 2

            Repeater {
                model: root.sectionItems

                Item {
                    id: indexEntry
                    required property var modelData
                    required property int index
                    readonly property bool current: root.currentSection === index
                    objectName: "settingsIndex_" + index
                    width: sectionIndex.width
                    height: 30
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.title
                    Accessible.description: "Go to section"
                    Accessible.onPressAction: root.showSection(index)
                    Keys.onPressed: function (event) {
                        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                || event.key === Qt.Key_Space) {
                            root.showSection(index)
                            event.accepted = true
                        }
                    }

                    Rectangle {
                        anchors.fill: parent
                        radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 6) : 3
                        color: indexEntry.current ? root.shade(Theme.accent, 0.18)
                               : indexHover.hovered ? root.shade(Theme.foreground, 0.08) : "transparent"
                        border.width: indexEntry.activeFocus ? 2 : 0
                        border.color: Theme.accent
                    }
                    Rectangle {
                        visible: indexEntry.current
                        x: 0
                        width: 3
                        height: parent.height - 12
                        anchors.verticalCenter: parent.verticalCenter
                        radius: 1
                        color: Theme.accent
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: indexEntry.modelData.title
                        elide: Text.ElideRight
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        font.weight: indexEntry.current ? Font.DemiBold : Font.Normal
                        color: indexEntry.current ? Theme.brightForeground : Theme.foreground
                    }
                    HoverHandler {
                        id: indexHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler { onTapped: root.showSection(indexEntry.index) }
                }
            }
        }

        Rectangle {
            visible: card.wide
            anchors.top: headerRule.bottom
            anchors.bottom: footerRule.top
            x: sectionIndex.x + sectionIndex.width + 13
            width: 1
            color: root.shade(Theme.foreground, 0.12)
        }

        Flickable {
            id: settingsFlickable
            objectName: "settingsScroll"
            onContentHeightChanged: if (root.visible) Qt.callLater(settingsFlickable.revealFocusedControl)
            onHeightChanged: if (root.visible) Qt.callLater(settingsFlickable.revealFocusedControl)
            anchors.top: headerRule.bottom
            anchors.bottom: footerRule.top
            anchors.left: parent.left
            anchors.leftMargin: card.wide ? sectionIndex.x + sectionIndex.width + 28 : 22
            anchors.right: parent.right
            anchors.rightMargin: 10
            contentHeight: column.implicitHeight + 36
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            onMovementStarted: root.requestedSection = -1
            ScrollBar.vertical: ScrollBar {
                id: settingsScrollBar
                onPressedChanged: if (pressed) root.requestedSection = -1
                width: 8
                policy: ScrollBar.AsNeeded
            }

            function revealFocusedControl() {
                const focused = root.Window.window ? root.Window.window.activeFocusItem : null
                let ancestor = focused
                while (ancestor && ancestor !== column) ancestor = ancestor.parent
                if (!focused || ancestor !== column) return
                const top = focused.mapToItem(column, 0, 0).y + column.y
                const bottom = top + focused.height
                // Clear of the fades at either edge.
                const inset = 24
                const maximum = Math.max(0, settingsFlickable.contentHeight - settingsFlickable.height)
                if (top - inset < settingsFlickable.contentY) {
                    settingsFlickable.contentY = Math.max(0, top - inset)
                    root.requestedSection = -1
                } else if (bottom + inset > settingsFlickable.contentY + settingsFlickable.height) {
                    settingsFlickable.contentY = Math.min(maximum, bottom + inset - settingsFlickable.height)
                    root.requestedSection = -1
                }
            }

            Connections {
                target: root.Window.window
                function onActiveFocusItemChanged() {
                    if (root.visible) Qt.callLater(settingsFlickable.revealFocusedControl)
                }
            }

            Column {
                id: column
                y: 18
                width: Math.min(settingsFlickable.width - settingsScrollBar.width - 18, 680)
                spacing: 34

            SettingsSection {
                id: librarySection
                objectName: "settingsSection_library"
                title: "Library"

                Column {
                    width: parent.width
                    spacing: 8

                    SettingRow {
                        title: "Automatic folders"
                        detail: "Detected from Omarchy and your XDG folders. New media appears here "
                                + "automatically; nothing is imported or copied."
                    }

                    Repeater {
                        model: Library.automaticFolders

                        ListedPath {
                            required property var modelData
                            label: modelData.label
                            path: modelData.path + (modelData.available ? "" : "  ·  Waiting for folder")
                            available: modelData.available
                        }
                    }
                }

                Column {
                    width: parent.width
                    spacing: 8

                    SettingRow {
                        title: "Additional folders"
                        detail: root.folderMessage !== "" ? root.folderMessage
                                : (Settings.libraryFolders.length === 0
                                   ? "Add another folder to this library."
                                   : Settings.libraryFolders.length + " added to this library.")
                        detailColor: root.folderMessage !== "" ? Theme.red : Theme.mutedText

                        PillButton {
                            id: addFolder
                            anchors.verticalCenter: parent.verticalCenter
                            label: "Add folder"
                            onClicked: { root.pinSelectedFolder = false; folderDialog.open() }
                        }
                    }

                    Repeater {
                        model: Settings.libraryFolders

                        Row {
                            id: folderRow
                            required property string modelData
                            readonly property bool available: {
                                void Library.automaticFolders
                                return Library.folderAvailable(modelData)
                            }
                            width: parent ? parent.width : 0
                            spacing: 10

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - removeFolder.width - 10
                                text: folderRow.modelData
                                      + (folderRow.available ? "" : "  ·  Unavailable")
                                elide: Text.ElideMiddle
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                color: folderRow.available ? Theme.mutedText : Theme.red
                            }
                            PillButton {
                                id: removeFolder
                                label: "Remove"
                                accessibleName: "Remove " + folderRow.modelData
                                onClicked: {
                                    if (Captures.folderFilter === folderRow.modelData) {
                                        Captures.folderFilter = ""
                                    }
                                    Settings.removeLibraryFolder(folderRow.modelData)
                                }
                            }
                        }
                    }
                }

                SettingRow {
                    title: "Pinned folders"
                    detail: "Pinned folders appear as shortcuts above the library."

                    PillButton {
                        id: pinFolder
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Pin folder"
                        onClicked: { root.pinSelectedFolder = true; folderDialog.open() }
                    }
                }

                SettingRow {
                    title: "Include Downloads"
                    detail: "Media files in your Downloads folder, top level only."
                    toggle: toggleDownloads

                    SettingSwitch {
                        id: toggleDownloads
                        objectName: "scanDownloadsToggle"
                        accessibleName: "Scan Downloads"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.scanDownloads
                        onToggled: Settings.scanDownloads = !Settings.scanDownloads
                    }
                }

                SettingRow {
                    title: "Folder depth"
                    detail: "How far to look inside Pictures and Videos. Screenshots and "
                            + "recordings are always read from the top level, where Omarchy "
                            + "writes them."

                    PillButton {
                        id: depthButton
                        objectName: "folderDepthButton"
                        accessibleName: "Folder scan depth: " + label
                        toolTip: "Click for the next depth, from 1 to 8"
                        anchors.verticalCenter: parent.verticalCenter
                        label: Settings.recursionDepth + " deep"
                        onClicked: Settings.recursionDepth =
                                   Settings.recursionDepth >= 8 ? 1 : Settings.recursionDepth + 1
                    }
                }

                SettingRow {
                    title: "Show hidden media"
                    detail: "Hiding is a \"don't show me this again\" mark. It never touches "
                            + "the file."
                    toggle: hiddenButton

                    SettingSwitch {
                        id: hiddenButton
                        objectName: "showHiddenToggle"
                        accessibleName: "Hidden items"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Captures.showHidden
                        onToggled: Captures.showHidden = !Captures.showHidden
                    }
                }

                SettingRow {
                    title: "Show filenames in grid"
                    detail: "Keep file names visible on thumbnails."
                    toggle: filenamesButton

                    SettingSwitch {
                        id: filenamesButton
                        objectName: "gridFilenamesToggle"
                        accessibleName: "Show filenames in grid"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.showGridFilenames
                        onToggled: Settings.showGridFilenames = !Settings.showGridFilenames
                    }
                }

                SettingRow {
                    title: "Group matching RAW and JPEG files"
                    detail: "A camera's RAW and JPEG of the same shot show as one tile."
                    toggle: pairingToggle

                    SettingSwitch {
                        id: pairingToggle
                        accessibleName: "Pair RAW and JPEG"
                        objectName: "pairRawJpegToggle"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.pairRawJpeg
                        onToggled: Settings.pairRawJpeg = !Settings.pairRawJpeg
                    }
                }

                SettingRow {
                    title: "Thumbnail cache"
                    detail: "Disk space for fast library previews."

                    PillButton {
                        id: cacheButton
                        objectName: "thumbnailCacheButton"
                        accessibleName: "Thumbnail cache size: " + label
                        toolTip: "Click for the next size, from 64 MB to 1 GB"
                        anchors.verticalCenter: parent.verticalCenter
                        label: root.cacheLabel(Settings.thumbnailCacheMb)
                        onClicked: Settings.thumbnailCacheMb = root.nextValue(
                                       [64, 128, 256, 512, 1024], Settings.thumbnailCacheMb)
                    }
                }

                SettingRow {
                    title: "Confirm permanent deletion"
                    detail: "Shift+Delete skips Trash and cannot be undone. Off deletes immediately. "
                            + "Regular Delete still uses Trash."
                    toggle: permanentDeleteConfirmation

                    SettingSwitch {
                        id: permanentDeleteConfirmation
                        objectName: "permanentDeleteConfirmation"
                        accessibleName: "Confirm permanent deletion"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.confirmPermanentDelete
                        onToggled: Settings.confirmPermanentDelete = !Settings.confirmPermanentDelete
                    }
                }
            }

            SettingsSection {
                id: viewerSection
                objectName: "settingsSection_viewer"
                title: "Viewer"

                SettingRow {
                    title: "Image default"
                    detail: "What Space does on a photo or screenshot."

                    PillButton {
                        id: imageActionButton
                        objectName: "imageDefaultButton"
                        accessibleName: "Default picture action: " + label
                        toolTip: "Click for the next action"
                        anchors.verticalCenter: parent.verticalCenter
                        label: root.actionLabel(Settings.imagePrimaryAction)
                        onClicked: Settings.imagePrimaryAction = root.nextValue(
                                       root.availableDefaults(["preview", "omaframe", "corrections", "matte", "view", "edit"]), Settings.imagePrimaryAction)
                    }
                }

                SettingRow {
                    title: "Enlarge small pictures to fit"
                    detail: "Off shows a picture smaller than the window at its own size."
                    toggle: enlargeButton

                    SettingSwitch {
                        id: enlargeButton
                        objectName: "enlargeSmallPicturesButton"
                        accessibleName: "Enlarge small pictures to fit"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.enlargeSmallPictures
                        onToggled: Settings.enlargeSmallPictures = !Settings.enlargeSmallPictures
                    }
                }

                SettingRow {
                    title: "Slideshow interval"
                    detail: "How long each picture stays before the next."

                    PillButton {
                        id: slideshowIntervalButton
                        objectName: "slideshowIntervalButton"
                        accessibleName: "Slideshow interval: " + label
                        toolTip: "Click for the next interval, from 2 to 20 seconds"
                        anchors.verticalCenter: parent.verticalCenter
                        label: Settings.slideshowIntervalSeconds + " s"
                        onClicked: {
                            const stops = [2, 3, 4, 5, 8, 12, 20]
                            const next = (stops.indexOf(Settings.slideshowIntervalSeconds) + 1)
                                         % stops.length
                            Settings.slideshowIntervalSeconds = stops[next]
                        }
                    }
                }

                SettingRow {
                    title: "Shuffle slideshow"
                    detail: "Pick a random picture each time instead of the next in order."
                    toggle: slideshowShuffleButton

                    SettingSwitch {
                        id: slideshowShuffleButton
                        objectName: "slideshowShuffleToggle"
                        accessibleName: "Shuffle slideshow"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.slideshowShuffle
                        onToggled: Settings.slideshowShuffle = !Settings.slideshowShuffle
                    }
                }
            }

            SettingsSection {
                id: videoSection
                objectName: "settingsSection_video"
                title: "Video"

                SettingRow {
                    title: "Video default"
                    detail: "What Space does on a video or recording."

                    PillButton {
                        id: videoActionButton
                        objectName: "videoDefaultButton"
                        accessibleName: "Default video action: " + label
                        toolTip: "Click for the next action"
                        anchors.verticalCenter: parent.verticalCenter
                        label: root.actionLabel(Settings.videoPrimaryAction)
                        onClicked: Settings.videoPrimaryAction = root.nextValue(
                                       root.availableDefaults(["preview", "trim", "play"]), Settings.videoPrimaryAction)
                    }
                }

                SettingRow {
                    title: "Remember playback speed"
                    detail: "Use the last chosen speed when opening a video in the quick viewer."
                    toggle: rememberPlaybackSpeedButton

                    SettingSwitch {
                        id: rememberPlaybackSpeedButton
                        objectName: "rememberPlaybackSpeedButton"
                        accessibleName: "Remember playback speed"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.rememberPlaybackSpeed
                        onToggled: Settings.rememberPlaybackSpeed = !Settings.rememberPlaybackSpeed
                    }
                }

                SettingRow {
                    title: "Videos in slideshows"
                    detail: "Play each video through before moving to the next item."
                    toggle: slideshowVideosButton

                    SettingSwitch {
                        id: slideshowVideosButton
                        objectName: "slideshowVideosToggle"
                        accessibleName: "Include videos in slideshow"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: Settings.slideshowVideos
                        onToggled: Settings.slideshowVideos = !Settings.slideshowVideos
                    }
                }
            }

            SettingsSection {
                id: searchSection
                objectName: "settingsSection_search"
                title: "Search and text"

                SettingRow {
                    title: "Picture text search"
                    detail: root.textCacheMessage !== "" ? root.textCacheMessage
                            : (TextIndex.available
                               ? "Tesseract text stays in a private local cache."
                               : "Tesseract is unavailable. Filename search still works.")

                    PillButton {
                        id: clearTextCacheButton
                        accessibleName: "Clear image text cache"
                        objectName: "clearTextCacheButton"
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Clear cache"
                        onClicked: {
                            // PDF text is cached beside picture text and goes with it.
                            const count = TextIndex.clearCache()
                                          + (typeof DocumentText !== "undefined"
                                             ? DocumentText.clearCache() : 0)
                            root.textCacheMessage = count === 0 ? "The text cache is already empty."
                                                               : "Cleared " + count
                                                                 + (count === 1 ? " entry." : " entries.")
                        }
                    }
                }

                TextIndexSettings { width: parent.width }
            }

            SettingsSection {
                id: albumsSection
                objectName: "settingsSection_albums"
                title: "Albums and backups"

                Text {
                    width: parent.width
                    visible: Settings.organizationError !== ""
                    text: Settings.organizationError
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.red
                }

                SettingRow {
                    visible: Settings.canRestoreOrganization
                    title: root.confirmingBackupRestore ? "" : "Daily backup"
                    detail: root.confirmingBackupRestore
                            ? "This replaces the albums, tags and marks still readable here. They are kept in a backup first."
                            : "Omaroll keeps a daily backup of your organization."
                    detailColor: root.confirmingBackupRestore ? Theme.red : Theme.mutedText

                    PillButton {
                        id: restoreBackupButton
                        objectName: "restoreBackupButton"
                        visible: !root.confirmingBackupRestore
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Restore last good backup"
                        onClicked: root.confirmingBackupRestore = true
                    }
                    PillButton {
                        id: restoreBackupConfirm
                        objectName: "restoreBackupConfirm"
                        visible: root.confirmingBackupRestore
                        anchors.verticalCenter: parent.verticalCenter
                        active: true
                        label: "Restore backup"
                        onClicked: {
                            root.confirmingBackupRestore = false
                            const result = Settings.restoreLastGoodBackup()
                            root.backupRestoreOk = result.ok
                            root.backupRestoreMessage = result.message
                            if (result.ok) {
                                Captures.setAlbumFilter("", [])
                                Captures.setTagFilter("", [])
                                Captures.clearSmartCollection()
                            }
                        }
                    }
                    PillButton {
                        id: restoreBackupCancel
                        objectName: "restoreBackupCancel"
                        visible: root.confirmingBackupRestore
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Cancel"
                        onClicked: root.confirmingBackupRestore = false
                    }
                }

                Text {
                    objectName: "backupRestoreMessage"
                    width: parent.width
                    visible: root.backupRestoreMessage !== ""
                    text: root.backupRestoreMessage
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: root.backupRestoreOk ? Theme.mutedText : Theme.red
                }

                Column {
                    width: parent.width
                    spacing: 8

                    SettingRow {
                        title: "Albums"
                        detail: Settings.albumNames.length === 0
                                ? "Create named collections without moving files."
                                : Settings.albumNames.length + (Settings.albumNames.length === 1
                                  ? " album" : " albums")

                        PillButton {
                            id: newAlbumButton
                            anchors.verticalCenter: parent.verticalCenter
                            label: "New album"
                            onClicked: root.createAlbumRequested()
                        }
                    }

                    Repeater {
                        model: Settings.albumNames

                        Row {
                            id: albumSettingsRow
                            required property string modelData
                            width: parent ? parent.width : 0
                            spacing: 10

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - deleteAlbumButton.width
                                       - (forgetUnavailableButton.visible
                                          ? forgetUnavailableButton.width + 10 : 0) - 10
                                text: albumSettingsRow.modelData + "  ·  "
                                      + Settings.albumPaths(albumSettingsRow.modelData).length + " of "
                                      + Settings.albumItemCount(albumSettingsRow.modelData) + " available"
                                elide: Text.ElideRight
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                color: Theme.mutedText
                            }
                            PillButton {
                                id: forgetUnavailableButton
                                visible: Settings.unavailableAlbumItemCount(albumSettingsRow.modelData) > 0
                                enabled: !Library.scanning
                                label: "Forget unavailable"
                                onClicked: Settings.removeUnavailableFromAlbum(albumSettingsRow.modelData)
                            }
                            PillButton {
                                id: deleteAlbumButton
                                label: "Delete"
                                accessibleName: "Delete album " + albumSettingsRow.modelData
                                onClicked: Settings.deleteAlbum(albumSettingsRow.modelData)
                            }
                        }
                    }
                }

                SettingRow {
                    title: "Back up and restore"
                    detail: root.pendingRestorePath !== ""
                            ? "Restore replaces the current organization. This cannot be undone."
                            : (root.organizationMessage !== "" ? root.organizationMessage
                               : "Save albums, tags, ratings, captions and saved views "
                                 + "to a file, or restore them. Media files are untouched.")
                    detailColor: (root.pendingRestorePath !== ""
                                  || (root.organizationMessage !== "" && !root.organizationOk))
                                 ? Theme.red : Theme.mutedText

                    PillButton {
                        id: backupButton
                        visible: root.pendingRestorePath === ""
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Back up"
                        onClicked: backupDialog.open()
                    }
                    PillButton {
                        id: restoreButton
                        visible: root.pendingRestorePath === ""
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Restore"
                        onClicked: restoreDialog.open()
                    }
                    PillButton {
                        visible: root.pendingRestorePath !== ""
                        anchors.verticalCenter: parent.verticalCenter
                        active: true
                        label: "Replace organization"
                        onClicked: root.applyPendingRestore()
                    }
                    PillButton {
                        visible: root.pendingRestorePath !== ""
                        anchors.verticalCenter: parent.verticalCenter
                        label: "Cancel"
                        onClicked: root.pendingRestorePath = ""
                    }
                }
            }

            SettingsSection {
                id: integrationSection
                objectName: "settingsSection_integration"
                title: "Integration"

                SettingRow {
                    objectName: "settingsDefaultViewer"
                    title: "Default viewer"
                    detail: root.defaultsStatus()
                    detailObjectName: "settingsDefaultViewerStatus"
                    detailColor: root.defaultsOk ? Theme.mutedText : Theme.red

                    PillButton {
                        objectName: "settingsSetDefault"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !(MimeDefaults.imagesDefault && MimeDefaults.videosDefault)
                        enabled: !MimeDefaults.busy
                        label: MimeDefaults.busy ? "Working…" : "Set as default"
                        toolTip: "Open pictures and videos with Omaroll from the file manager"
                        accessibleName: "Set as default for pictures and videos"
                        onClicked: MimeDefaults.applyAsync(false, "all")
                    }
                    PillButton {
                        objectName: "settingsRestoreDefault"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: MimeDefaults.imagesDefault || MimeDefaults.videosDefault
                        enabled: !MimeDefaults.busy
                        label: "Restore previous apps"
                        toolTip: "Put back the apps that opened them before"
                        onClicked: MimeDefaults.applyAsync(true, "all")
                    }
                }

                SettingRow {
                    objectName: "settingsWindowRules"
                    visible: root.windowRulesState !== "unsupported" || root.windowRulesMessage !== ""
                    title: "Hyprland window rules"
                    detailObjectName: "settingsWindowRulesStatus"
                    detail: root.windowRulesMessage !== "" ? root.windowRulesMessage
                            : root.windowRulesState === "installed"
                              ? "Omaroll's windows open at full opacity and the viewer floats."
                              : "Omarchy dims every window a little. These rules keep pictures at full "
                                + "opacity and float the viewer. Saved in ~/.config/hypr."

                    PillButton {
                        id: windowRulesButton
                        objectName: "settingsWindowRulesButton"
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.windowRulesState !== "unsupported"
                        label: root.windowRulesState === "installed" ? "Remove" : "Install"
                        toolTip: root.windowRulesState === "installed"
                                 ? "Take Omaroll's rules out of your Hyprland config"
                                 : "Add Omaroll's rules to your Hyprland config"
                        accessibleName: label + " Hyprland window rules"
                        onClicked: {
                            root.windowRulesMessage = root.windowRulesState === "installed"
                                    ? Settings.removeWindowRules() : Settings.installWindowRules()
                            root.windowRulesState = Settings.windowRules
                        }
                    }
                }
            }

            SettingsSection {
                id: aboutSection
                objectName: "settingsSection_about"
                title: "About"

                Column {
                    width: parent.width
                    spacing: 10

                    Text {
                        objectName: "aboutVersion"
                        text: "Omaroll " + Settings.appVersion + " · Qt " + Settings.qtVersion
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.foreground
                    }
                    Flow {
                        width: parent.width
                        spacing: 8
                        PillButton {
                            objectName: "aboutChangelog"
                            label: "Changelog"
                            toolTip: "What changed in Omaroll " + Settings.appVersion
                            property string url: Settings.releaseNotesUrl
                            onClicked: Qt.openUrlExternally(url)
                        }
                        PillButton {
                            objectName: "aboutHomepage"
                            label: "Homepage"
                            property string url: "https://btsouth.github.io/omaroll/"
                            onClicked: Qt.openUrlExternally(url)
                        }
                        PillButton {
                            objectName: "aboutReportIssue"
                            label: "Report an issue"
                            toolTip: "Copy diagnostics first, then paste them into the form"
                            property string url: Settings.newIssueUrl
                            onClicked: Qt.openUrlExternally(url)
                        }
                        PillButton {
                            id: copyDiagnosticsButton
                            objectName: "copyDiagnostics"
                            label: "Copy diagnostics"
                            toolTip: "omaroll --diagnostics prints the same text"
                            onClicked: Settings.copyDiagnostics(Theme.themeName, root.Screen.devicePixelRatio)
                            Connections {
                                target: Settings
                                function onDiagnosticsCopied() { copyDiagnosticsButton.label = "Diagnostics copied" }
                            }
                        }
                    }
                    Text {
                        objectName: "aboutMissingToolsTitle"
                        width: parent.width
                        visible: root.missingTools.length > 0
                        topPadding: 6
                        text: "Not installed"
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        color: Theme.foreground
                    }
                    Repeater {
                        objectName: "aboutMissingTools"
                        model: root.missingTools
                        delegate: Row {
                            required property var modelData
                            width: parent ? parent.width : 0
                            spacing: 10
                            Text {
                                width: Math.round(parent.width * 0.45)
                                elide: Text.ElideRight
                                text: modelData.feature
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                color: Theme.mutedText
                            }
                            TextEdit {
                                width: parent.width - Math.round(parent.width * 0.45) - 10
                                readOnly: true
                                selectByMouse: true
                                wrapMode: TextEdit.Wrap
                                text: modelData.install
                                font.family: Theme.fontFamily
                                font.pixelSize: 12
                                color: Theme.foreground
                                selectionColor: Theme.accent
                            }
                        }
                    }
                }
            }
            }
        }

        // Fades where the list runs on past either edge, instead of a hard cut.
        Rectangle {
            objectName: "settingsTopFade"
            anchors.top: settingsFlickable.top
            anchors.left: settingsFlickable.left
            anchors.right: settingsFlickable.right
            anchors.rightMargin: settingsScrollBar.width + 4
            height: 28
            visible: settingsFlickable.contentY > 1
            gradient: Gradient {
                GradientStop { position: 0.0; color: card.color }
                GradientStop { position: 1.0; color: root.shade(card.color, 0) }
            }
        }
        Rectangle {
            objectName: "settingsBottomFade"
            anchors.bottom: settingsFlickable.bottom
            anchors.left: settingsFlickable.left
            anchors.right: settingsFlickable.right
            anchors.rightMargin: settingsScrollBar.width + 4
            height: 36
            visible: settingsFlickable.contentY + settingsFlickable.height
                     < settingsFlickable.contentHeight - 1
            gradient: Gradient {
                GradientStop { position: 0.0; color: root.shade(card.color, 0) }
                GradientStop { position: 1.0; color: card.color }
            }
        }

        Rectangle {
            id: footerRule
            anchors.bottom: footer.top
            anchors.bottomMargin: 12
            width: parent.width
            height: 1
            color: root.shade(Theme.foreground, 0.12)
        }

        Row {
            id: footer
            anchors.right: parent.right
            anchors.rightMargin: 22
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            spacing: 8

            PillButton {
                objectName: "settingsRescan"
                label: "Rescan now"
                onClicked: {
                    root.rescanRequested()
                    root.close()
                }
            }

            PillButton {
                objectName: "settingsDone"
                label: "Done"
                active: true
                onClicked: root.close()
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: root.pinSelectedFolder ? "Pin a folder in Omaroll" : "Add a folder to Omaroll"
        onAccepted: {
            root.folderMessage = root.pinSelectedFolder
                                 ? (Settings.pinFolder(selectedFolder) ? "Folder pinned"
                                    : "That folder is already pinned or unavailable")
                                 : Settings.addLibraryFolder(selectedFolder)
                                 ? "Folder added"
                                 : "That folder is already added, unavailable, or is your home folder"
        }
    }

    FileDialog {
        id: backupDialog
        title: "Back up Omaroll organization"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "json"
        nameFilters: ["Omaroll backup (*.json)"]
        onAccepted: {
            const result = Settings.exportOrganization(root.localPath(selectedFile))
            root.organizationOk = result.ok
            root.organizationMessage = result.message
        }
    }

    FileDialog {
        id: restoreDialog
        title: "Restore Omaroll organization"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Omaroll backup (*.json)"]
        onAccepted: {
            // Chosen, not applied: the restore waits for confirmation below.
            root.pendingRestorePath = root.localPath(selectedFile)
            root.organizationMessage = ""
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        }
    }
}
