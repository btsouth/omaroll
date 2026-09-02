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
    property string textCacheMessage: ""

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function nextValue(values, current) {
        return values[(values.indexOf(current) + 1) % values.length]
    }

    function actionLabel(action) {
        const labels = { "matte": "Make postable", "view": "View full size",
                         "edit": "Edit in Pinta", "trim": "Trim", "play": "Play" }
        return labels[action]
    }

    function cacheLabel(megabytes) {
        return megabytes === 1024 ? "1 GB" : megabytes + " MB"
    }

    function open() {
        folderMessage = ""
        textCacheMessage = ""
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
                      + "It changes a file only when you explicitly rename or trash it."
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
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.scanDownloads ? "On" : "Off"
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
                    anchors.verticalCenter: parent.verticalCenter
                    label: Captures.showHidden ? "Shown" : "Hidden"
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
                    anchors.verticalCenter: parent.verticalCenter
                    label: Settings.slideshowVideos ? "On" : "Off"
                    active: Settings.slideshowVideos
                    onClicked: Settings.slideshowVideos = !Settings.slideshowVideos
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
                        text: "What Enter does on a photo or screenshot."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: imageActionButton
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.actionLabel(Settings.imagePrimaryAction)
                    onClicked: Settings.imagePrimaryAction = root.nextValue(
                                   ["matte", "view", "edit"], Settings.imagePrimaryAction)
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
                        text: "What Enter does on a video or recording."
                        font.family: Theme.fontFamily
                        font.pixelSize: 10
                        color: Theme.mutedText
                    }
                }

                PillButton {
                    id: videoActionButton
                    anchors.verticalCenter: parent.verticalCenter
                    label: root.actionLabel(Settings.videoPrimaryAction)
                    onClicked: Settings.videoPrimaryAction = root.nextValue(
                                   ["trim", "play"], Settings.videoPrimaryAction)
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
                    onClicked: folderDialog.open()
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
        title: "Add a folder to Omaroll"
        onAccepted: {
            root.folderMessage = Settings.addLibraryFolder(selectedFolder)
                                 ? "Folder added"
                                 : "That folder is already added, unavailable, or is your home folder"
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        }
    }
}
