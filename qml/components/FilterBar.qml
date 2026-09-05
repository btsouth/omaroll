import QtQuick
import QtQuick.Controls.Basic

// Kind filter, search and sort. One row, left to right in the order someone
// actually reaches for them: narrow what is shown, then find one thing, then
// change the order.
Item {
    id: root

    // Mirrors CaptureRecord::Kind. -1 is every kind.
    readonly property int kindAll: -1
    readonly property int kindScreenshot: 0
    readonly property int kindRecording: 1
    readonly property int kindPicture: 2
    readonly property int kindVideo: 3
    readonly property int kindDownload: 4
    readonly property int kindDocument: 5

    // Number keys keep the existing 7 shortcut for favourites. PDFs take 8.
    function selectSection(index) {
        if (index === 6) {
            Captures.favoritesOnly = !Captures.favoritesOnly
            return
        }
        if (index === 7) {
            Captures.kindFilter = kindDocument
            return
        }
        const kinds = [kindAll, kindScreenshot, kindRecording, kindPicture, kindVideo,
                       kindDownload]
        if (index === 5 && !Settings.scanDownloads) {
            return
        }
        Captures.kindFilter = kinds[index]
    }

    function sectionShortcut(index) { return String(index + 1) }

    // Tab walks the sections in the order the pills show them and wraps.
    // Favourites is orthogonal to the kind and stays out of the cycle.
    function cycleSection(step) {
        const kinds = [kindAll, kindScreenshot, kindRecording, kindPicture, kindVideo,
                       kindDocument]
        if (Settings.scanDownloads) {
            kinds.splice(5, 0, kindDownload)
        }
        const current = Math.max(0, kinds.indexOf(Captures.kindFilter))
        Captures.kindFilter = kinds[(current + step + kinds.length) % kinds.length]
    }

    readonly property var sortLabels: ["Newest", "Oldest", "Largest", "Smallest", "Name",
                                       "Top rated"]

    // True while the search field owns the keyboard, so window-level
    // shortcuts that the field would not swallow (Escape, Ctrl+H) stand down.
    readonly property bool searchActive: search.activeFocus
    // A menu is up. Main disables the surfaces under it: a modal popup stops
    // items from getting the press that closes it, but not the passive tap
    // handlers on the tiles, which would otherwise still fire.
    readonly property bool menuOpen: sortMenu.visible
    property bool browserOpen: false

    // The user left the search field with Enter or Escape.
    signal done()
    signal browseRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    function folderName(path) {
        const parts = path.split("/").filter(function (part) { return part !== "" })
        return parts.length > 0 ? parts[parts.length - 1] : path
    }

    // On a half-screen tile the pills and the search cannot share a row, so
    // the search and sort drop to a second line rather than overlap.
    readonly property bool separateBrowse: root.width < 620
    readonly property bool wrapped: root.width < kinds.width + 40 + 90 + sortButton.width + 60
    implicitHeight: wrapped ? 76 : 40

    Row {
        id: kinds
        anchors.left: parent.left
        anchors.leftMargin: 20
        anchors.top: parent.top
        anchors.topMargin: 7
        spacing: 6

        PillButton {
            label: "All"
            shortcut: root.sectionShortcut(0)
            active: Captures.kindFilter === root.kindAll
            onClicked: Captures.kindFilter = root.kindAll
        }
        PillButton {
            label: root.width < 700 ? "Shots" : "Screenshots"
            shortcut: root.sectionShortcut(1)
            active: Captures.kindFilter === root.kindScreenshot
            onClicked: Captures.kindFilter = root.kindScreenshot
        }
        PillButton {
            label: root.width < 700 ? "Clips" : "Recordings"
            shortcut: root.sectionShortcut(2)
            active: Captures.kindFilter === root.kindRecording
            onClicked: Captures.kindFilter = root.kindRecording
        }
        PillButton {
            label: root.width < 700 ? "Photos" : "Pictures"
            shortcut: root.sectionShortcut(3)
            active: Captures.kindFilter === root.kindPicture
            onClicked: Captures.kindFilter = root.kindPicture
        }
        PillButton {
            label: "Videos"
            shortcut: root.sectionShortcut(4)
            active: Captures.kindFilter === root.kindVideo
            onClicked: Captures.kindFilter = root.kindVideo
        }
        PillButton {
            visible: Settings.scanDownloads
            label: root.width < 700 ? "Down" : "Downloads"
            shortcut: root.sectionShortcut(5)
            active: Captures.kindFilter === root.kindDownload
            onClicked: Captures.kindFilter = root.kindDownload
        }
        PillButton {
            label: "PDFs"
            shortcut: root.sectionShortcut(7)
            active: Captures.kindFilter === root.kindDocument
            onClicked: Captures.kindFilter = root.kindDocument
        }

        Item { width: 6; height: 1 }

        // Orthogonal to the kind: favourites of whatever section is showing.
        PillButton {
            label: "★"
            toolTip: "Favourites"
            shortcut: root.sectionShortcut(6)
            active: Captures.favoritesOnly
            onClicked: Captures.favoritesOnly = !Captures.favoritesOnly
        }

        PillButton {
            id: libraryButton
            // Keep Browse reachable when the kind filters fill a narrow row.
            parent: root.separateBrowse ? root : kinds
            x: root.separateBrowse ? 20 : 0
            y: root.separateBrowse ? 43 : 0
            label: root.width < 700 ? (Captures.duplicatesOnly ? "Dupes  ▾"
                                      : Captures.similarOnly ? "Similar  ▾" : "Browse  ▾")
                   : (Captures.duplicatesOnly ? "Duplicates  ▾"
                      : Captures.similarOnly ? "Similar pictures  ▾"
                      : Captures.smartCollectionFilter !== "" ? Captures.smartCollectionFilter + "  ▾"
                      : Captures.tagFilter !== "" ? "#" + Captures.tagFilter + "  ▾"
                      : Captures.cameraFilter !== "" ? Captures.cameraFilter + "  ▾"
                      : Captures.lensFilter !== "" ? Captures.lensFilter + "  ▾"
                      : Captures.minimumRating > 0
                        ? "★".repeat(Captures.minimumRating) + "+  ▾"
                      : Captures.dateFrom !== "" || Captures.modifiedAfter !== "" ? "Date  ▾"
                      : Captures.albumFilter !== "" ? Captures.albumFilter + "  ▾"
                      : Captures.folderFilter !== ""
                        ? root.folderName(Captures.folderFilter) + "  ▾" : "Browse  ▾")
            active: Captures.folderFilter !== "" || Captures.albumFilter !== ""
                    || Captures.duplicatesOnly
                    || Captures.similarOnly || Captures.tagFilter !== ""
                    || Captures.cameraFilter !== "" || Captures.lensFilter !== ""
                    || Captures.minimumRating > 0
                    || Captures.dateFrom !== "" || Captures.modifiedAfter !== ""
                    || Captures.smartCollectionFilter !== ""
                    || root.browserOpen
            toolTip: Captures.folderFilter
            onClicked: root.browseRequested()
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 20
        anchors.top: parent.top
        anchors.topMargin: root.wrapped ? 43 : 7
        spacing: 8

        // Search. Gives up width before it runs into the kind pills.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: root.wrapped ? Math.max(120, Math.min(260, root.width - sortButton.width - 60))
                                : Math.max(120, Math.min(200, root.width - kinds.width - sortButton.width - 70))
            height: 26
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            color: root.shade(Theme.foreground, search.activeFocus ? 0.10 : 0.05)
            border.width: 1
            border.color: search.activeFocus
                          ? root.shade(Theme.accent, 0.6)
                          : root.shade(Theme.foreground, 0.12)

            Behavior on color { ColorAnimation { duration: 140 } }
            Behavior on border.color { ColorAnimation { duration: 140 } }

            TextInput {
                id: search
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                verticalAlignment: TextInput.AlignVCenter
                clip: true
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.foreground
                selectionColor: root.shade(Theme.accent, 0.5)
                selectedTextColor: Theme.brightForeground

                onTextChanged: Captures.searchText = text
                Keys.onEscapePressed: {
                    text = ""
                    root.done()
                }
                Keys.onReturnPressed: root.done()
                Keys.onEnterPressed: root.done()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: 10
                visible: search.text === "" && !search.activeFocus
                text: TextIndex.available ? "Search names + text  /" : "Search  /"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: root.shade(Theme.foreground, 0.35)
            }
        }

        // Sort. A menu rather than five more pills, because the sort is changed
        // far less often than the filter and does not deserve equal weight.
        PillButton {
            id: sortButton
            anchors.verticalCenter: parent.verticalCenter
            label: root.sortLabels[Captures.sortMode] + "  ▾"
            active: sortMenu.visible
            onClicked: sortMenu.visible ? sortMenu.close() : sortMenu.popup(sortButton, 0, sortButton.height + 4)
        }
    }

    Menu {
        id: sortMenu
        objectName: "sortMenu"
        // Modal, undimmed: the press that closes the menu is consumed here
        // rather than also landing on the tile or pill under it.
        modal: true
        dim: false

        background: Rectangle {
            implicitWidth: 150
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        Repeater {
            model: root.sortLabels

            MenuItem {
                id: sortRow
                required property int index
                required property string modelData

                height: 30

                contentItem: Text {
                    text: sortRow.modelData
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Captures.sortMode === sortRow.index ? Font.DemiBold : Font.Normal
                    color: Captures.sortMode === sortRow.index ? Theme.accent : Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                }

                background: Rectangle {
                    color: sortRow.hovered ? root.shade(Theme.foreground, 0.08) : "transparent"
                }

                onTriggered: Captures.sortMode = sortRow.index
            }
        }
    }
}
