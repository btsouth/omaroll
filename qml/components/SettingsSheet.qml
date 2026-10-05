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
    property bool organizationOk: false
    // Set once a backup file has been chosen; the restore runs only after the
    // user confirms, since it replaces the current organization.
    property string pendingRestorePath: ""

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
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
        visible = true
        forceActiveFocus()
    }

    function close() { visible = false }

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
            const delta = event.pixelDelta.y !== 0
                          ? event.pixelDelta.y : event.angleDelta.y / 2
            const minimum = settingsFlickable.originY
            const maximum = minimum + Math.max(
                                0, settingsFlickable.contentHeight - settingsFlickable.height)
            settingsFlickable.contentY = Math.max(
                        minimum, Math.min(maximum, settingsFlickable.contentY - delta))
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

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(520, root.width - 60)
        height: Math.min(620, root.height - 60, column.implicitHeight + 44)
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

        Flickable {
            id: settingsFlickable
            objectName: "settingsScroll"
            onContentHeightChanged: if (root.visible) Qt.callLater(settingsFlickable.revealFocusedControl)
            onHeightChanged: if (root.visible) Qt.callLater(settingsFlickable.revealFocusedControl)
            anchors.fill: parent
            anchors.margins: 22
            contentHeight: column.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {
                id: settingsScrollBar
                width: 8
                policy: ScrollBar.AsNeeded
            }

            function revealFocusedControl() {
                const focused = root.Window.window ? root.Window.window.activeFocusItem : null
                let ancestor = focused
                while (ancestor && ancestor !== column) ancestor = ancestor.parent
                if (!focused || ancestor !== column) return
                const top = focused.mapToItem(column, 0, 0).y
                const bottom = top + focused.height
                const maximum = Math.max(0, settingsFlickable.contentHeight - settingsFlickable.height)
                if (top < settingsFlickable.contentY) settingsFlickable.contentY = Math.max(0, top)
                else if (bottom > settingsFlickable.contentY + settingsFlickable.height)
                    settingsFlickable.contentY = Math.min(maximum, bottom - settingsFlickable.height)
            }

            Connections {
                target: root.Window.window
                function onActiveFocusItemChanged() {
                    if (root.visible) Qt.callLater(settingsFlickable.revealFocusedControl)
                }
            }

            Column {
                id: column
                width: parent.width - settingsScrollBar.width - 14
                spacing: 14

            Text {
                text: "Settings"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: "Omaroll reads the folders Omarchy already writes captures to. "
                      + "Rename and deletion happen only when you request them."
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Column {
                width: parent.width
                spacing: 7

                Text {
                    text: "Automatic folders"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.foreground
                }
                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: "Detected from Omarchy and your XDG folders. New media appears here "
                          + "automatically; nothing is imported or copied."
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    color: Theme.mutedText
                }

                Repeater {
                    model: Library.automaticFolders

                    Column {
                        required property var modelData
                        width: parent.width
                        spacing: 1

                        Text {
                            width: parent.width
                            text: modelData.label
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            color: Theme.foreground
                        }
                        Text {
                            width: parent.width
                            text: modelData.path + (modelData.available ? "" : "  ·  Waiting for folder")
                            elide: Text.ElideMiddle
                            font.family: Theme.fontFamily
                            font.pixelSize: 10
                            color: modelData.available ? Theme.mutedText : Theme.red
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            // Downloads
            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - toggleDownloads.width - 12
                    spacing: 2

                    Text {
                        text: "Include Downloads"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Media files in your Downloads folder, top level only."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: toggleDownloads
                    accessibleName: "Scan Downloads"
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.scanDownloads ? "On" : "Off"
                    checkable: true
                    active: Settings.scanDownloads
                    onClicked: Settings.scanDownloads = !Settings.scanDownloads
                }
            }

            // Recursion depth
            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - depthButton.width - 12
                    spacing: 2

                    Text {
                        text: "Folder depth"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "How far to look inside Pictures and Videos. Screenshots and "
                              + "recordings are always read from the top level, where Omarchy "
                              + "writes them."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: depthButton
                    accessibleName: "Folder scan depth: " + label
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.recursionDepth + " deep"
                    onClicked: Settings.recursionDepth =
                               Settings.recursionDepth >= 8 ? 1 : Settings.recursionDepth + 1
                }
            }

            // Hidden
            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - hiddenButton.width - 12
                    spacing: 2

                    Text {
                        text: "Show hidden media"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Hiding is a \"don't show me this again\" mark. It never touches "
                              + "the file."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: hiddenButton
                    accessibleName: "Hidden items"
                    anchors.verticalCenter: parent.verticalCenter
                    label: Captures.showHidden ? "Shown" : "Hidden"
                    checkable: true
                    active: Captures.showHidden
                    onClicked: Captures.showHidden = !Captures.showHidden
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - clearTextCacheButton.width - 12
                    spacing: 2

                    Text {
                        text: "Picture text search"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: root.textCacheMessage !== "" ? root.textCacheMessage
                              : (TextIndex.available
                                 ? "Tesseract text stays in a private local cache."
                                 : "Tesseract is unavailable. Filename search still works.")
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: clearTextCacheButton
                    accessibleName: "Clear image text cache"
                    objectName: "clearTextCacheButton"
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Clear cache"
                    onClicked: {
                        const count = TextIndex.clearCache()
                        root.textCacheMessage = count === 0 ? "The text cache is already empty."
                                                           : "Cleared " + count
                                                             + (count === 1 ? " entry." : " entries.")
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Row {
                width: parent.width
                spacing: 12
                Column {
                    width: parent.width - permanentDeleteConfirmation.width - 12
                    spacing: 2
                    Text {
                        text: "Confirm permanent deletion"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Shift+Delete skips Trash and cannot be undone. Off deletes immediately. Regular Delete still uses Trash."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }
                PillButton {
                    id: permanentDeleteConfirmation
                    objectName: "permanentDeleteConfirmation"
                    accessibleName: "Confirm permanent deletion"
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.confirmPermanentDelete ? "On" : "Off"
                    checkable: true
                    active: Settings.confirmPermanentDelete
                    onClicked: Settings.confirmPermanentDelete = !Settings.confirmPermanentDelete
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - rememberPlaybackSpeedButton.width - 12
                    spacing: 2

                    Text {
                        text: "Remember playback speed"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Use the last chosen speed when opening a video in the quick viewer."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: rememberPlaybackSpeedButton
                    objectName: "rememberPlaybackSpeedButton"
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.rememberPlaybackSpeed ? "On" : "Off"
                    active: Settings.rememberPlaybackSpeed
                    onClicked: Settings.rememberPlaybackSpeed = !Settings.rememberPlaybackSpeed
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - slideshowVideosButton.width - 12
                    spacing: 2

                    Text {
                        text: "Videos in slideshows"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Play each video through before moving to the next item."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: slideshowVideosButton
                    accessibleName: "Include videos in slideshow"
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.slideshowVideos ? "On" : "Off"
                    checkable: true
                    active: Settings.slideshowVideos
                    onClicked: Settings.slideshowVideos = !Settings.slideshowVideos
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - slideshowIntervalButton.width - 12
                    spacing: 2

                    Text {
                        text: "Slideshow interval"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "How long each picture stays before the next."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: slideshowIntervalButton
                    accessibleName: "Slideshow interval: " + label
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

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - slideshowShuffleButton.width - 12
                    spacing: 2

                    Text {
                        text: "Shuffle slideshow"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Pick a random picture each time instead of the next in order."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: slideshowShuffleButton
                    accessibleName: "Shuffle slideshow"
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.slideshowShuffle ? "On" : "Off"
                    checkable: true
                    active: Settings.slideshowShuffle
                    onClicked: Settings.slideshowShuffle = !Settings.slideshowShuffle
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - newAlbumButton.width - 12
                    spacing: 2

                    Text {
                        text: "Albums"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: Settings.albumNames.length === 0
                              ? "Create named collections without moving files."
                              : Settings.albumNames.length + (Settings.albumNames.length === 1
                                ? " album" : " albums")
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

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
                    width: column.width
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
                        font.pixelSize: 10
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
                        onClicked: Settings.deleteAlbum(albumSettingsRow.modelData)
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - imageActionButton.width - 12
                    spacing: 2

                    Text {
                        text: "Image default"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "What Space does on a photo or screenshot."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: imageActionButton
                    accessibleName: "Default picture action: " + label
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.actionLabel(Settings.imagePrimaryAction)
                    onClicked: Settings.imagePrimaryAction = root.nextValue(
                                   root.availableDefaults(["preview", "omaframe", "corrections", "matte", "view", "edit"]), Settings.imagePrimaryAction)
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - videoActionButton.width - 12
                    spacing: 2

                    Text {
                        text: "Video default"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "What Space does on a video or recording."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: videoActionButton
                    accessibleName: "Default video action: " + label
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.actionLabel(Settings.videoPrimaryAction)
                    onClicked: Settings.videoPrimaryAction = root.nextValue(
                                   root.availableDefaults(["preview", "trim", "play"]), Settings.videoPrimaryAction)
                }
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - cacheButton.width - 12
                    spacing: 2

                    Text {
                        text: "Thumbnail cache"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: "Disk space for fast library previews."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: cacheButton
                    accessibleName: "Thumbnail cache size: " + label
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.cacheLabel(Settings.thumbnailCacheMb)
                    onClicked: Settings.thumbnailCacheMb = root.nextValue(
                                   [64, 128, 256, 512, 1024], Settings.thumbnailCacheMb)
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - addFolder.width - 12
                    spacing: 2

                    Text {
                        text: "Additional folders"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: root.folderMessage !== "" ? root.folderMessage
                              : (Settings.libraryFolders.length === 0
                                 ? "Add another folder to this library."
                                 : Settings.libraryFolders.length + " added to this library.")
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: root.folderMessage !== "" ? Theme.red : Theme.mutedText
                    }
                }

                PillButton {
                    id: addFolder
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Add folder"
                    onClicked: { root.pinSelectedFolder = false; folderDialog.open() }
                }
            }

            Row {
                width: parent.width
                spacing: 12
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - pinFolder.width - 12
                    text: "Pinned folders appear as shortcuts above the library."
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 10
                    color: Theme.mutedText
                }
                PillButton {
                    id: pinFolder
                    label: "Pin folder"
                    onClicked: { root.pinSelectedFolder = true; folderDialog.open() }
                }
            }

            Row {
                width: parent.width
                spacing: 12
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - pairingToggle.width - 12
                    text: "Group matching RAW and JPEG files"
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: Theme.foreground
                }
                PillButton {
                    id: pairingToggle
                    accessibleName: "Pair RAW and JPEG"
                    objectName: "pairRawJpegToggle"
                    label: Settings.pairRawJpeg ? "On" : "Off"
                    checkable: true
                    active: Settings.pairRawJpeg
                    onClicked: Settings.pairRawJpeg = !Settings.pairRawJpeg
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
                    width: column.width
                    spacing: 10

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - removeFolder.width - 10
                        text: folderRow.modelData
                              + (folderRow.available ? "" : "  ·  Unavailable")
                        elide: Text.ElideMiddle
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: folderRow.available ? Theme.mutedText : Theme.red
                    }
                    PillButton {
                        id: removeFolder
                        label: "Remove"
                        onClicked: {
                            if (Captures.folderFilter === folderRow.modelData) {
                                Captures.folderFilter = ""
                            }
                            Settings.removeLibraryFolder(folderRow.modelData)
                        }
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Rectangle {
                width: parent.width
                height: 1
                color: root.shade(Theme.foreground, 0.12)
            }

            Row {
                width: parent.width
                spacing: 12

                Column {
                    width: parent.width - backupButton.width - restoreButton.width - 24
                    spacing: 2

                    Text {
                        text: "Back up and restore"
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.foreground
                    }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        text: root.pendingRestorePath !== ""
                              ? "Restore replaces the current organization. This cannot be undone."
                              : (root.organizationMessage !== "" ? root.organizationMessage
                                 : "Save albums, tags, ratings, captions and saved views "
                                   + "to a file, or restore them. Media files are untouched.")
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: (root.pendingRestorePath !== ""
                                || (root.organizationMessage !== "" && !root.organizationOk))
                               ? Theme.red : Theme.mutedText
                    }
                }

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

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    label: "Rescan now"
                    onClicked: {
                        root.rescanRequested()
                        root.close()
                    }
                }

                PillButton {
                    label: "Done"
                    active: true
                    onClicked: root.close()
                }
            }
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
