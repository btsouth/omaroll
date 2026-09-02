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

    // Sections in bar order, for the number keys.
    function selectSection(index) {
        if (index === 6) {
            Captures.favoritesOnly = !Captures.favoritesOnly
            return
        }
        const kinds = [kindAll, kindScreenshot, kindRecording, kindPicture, kindVideo, kindDownload]
        if (index === 5 && !Settings.scanDownloads) {
            return
        }
        Captures.kindFilter = kinds[index]
    }

    readonly property var sortLabels: ["Newest", "Oldest", "Largest", "Smallest", "Name"]

    // True while the search field owns the keyboard, so window-level
    // shortcuts that the field would not swallow (Escape, Ctrl+H) stand down.
    readonly property bool searchActive: search.activeFocus
    // A menu is up. Main disables the surfaces under it: a modal popup stops
    // items from getting the press that closes it, but not the passive tap
    // handlers on the tiles, which would otherwise still fire.
    readonly property bool menuOpen: libraryMenu.visible || sortMenu.visible

    // The user left the search field with Enter or Escape.
    signal done()
    signal createAlbumRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    function shortFolder(path) {
        const parts = path.split("/").filter(function (part) { return part !== "" })
        return parts.slice(Math.max(0, parts.length - 2)).join("/")
    }

    // On a half-screen tile the pills and the search cannot share a row, so
    // the search and sort drop to a second line rather than overlap.
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
            active: Captures.kindFilter === root.kindAll
            onClicked: Captures.kindFilter = root.kindAll
        }
        PillButton {
            label: root.width < 700 ? "Shots" : "Screenshots"
            active: Captures.kindFilter === root.kindScreenshot
            onClicked: Captures.kindFilter = root.kindScreenshot
        }
        PillButton {
            label: root.width < 700 ? "Clips" : "Recordings"
            active: Captures.kindFilter === root.kindRecording
            onClicked: Captures.kindFilter = root.kindRecording
        }
        PillButton {
            label: root.width < 700 ? "Photos" : "Pictures"
            active: Captures.kindFilter === root.kindPicture
            onClicked: Captures.kindFilter = root.kindPicture
        }
        PillButton {
            label: "Videos"
            active: Captures.kindFilter === root.kindVideo
            onClicked: Captures.kindFilter = root.kindVideo
        }
        PillButton {
            visible: Settings.scanDownloads
            label: root.width < 700 ? "Down" : "Downloads"
            active: Captures.kindFilter === root.kindDownload
            onClicked: Captures.kindFilter = root.kindDownload
        }

        Item { width: 6; height: 1 }

        // Orthogonal to the kind: favourites of whatever section is showing.
        PillButton {
            label: "★"
            active: Captures.favoritesOnly
            onClicked: Captures.favoritesOnly = !Captures.favoritesOnly
        }

        PillButton {
            id: libraryButton
            label: root.width < 700 ? "Browse  ▾"
                   : (Captures.albumFilter !== "" ? Captures.albumFilter + "  ▾"
                      : Captures.folderFilter !== ""
                        ? root.shortFolder(Captures.folderFilter) + "  ▾" : "Browse  ▾")
            active: Captures.folderFilter !== "" || Captures.albumFilter !== ""
                    || libraryMenu.visible
            onClicked: libraryMenu.visible ? libraryMenu.close()
                                           : libraryMenu.popup(libraryButton, 0,
                                                               libraryButton.height + 4)
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
                text: "Search  /"
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
        id: libraryMenu
        objectName: "libraryMenu"
        // Modal, undimmed: the press that closes the menu is consumed here
        // rather than also landing on the tile or pill under it.
        modal: true
        dim: false

        background: Rectangle {
            implicitWidth: 280
            color: root.shade(Theme.background, 0.97)
            border.width: 1
            border.color: root.shade(Theme.foreground, 0.16)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
        }

        MenuItem {
            id: allMediaRow
            height: 30
            contentItem: Text {
                text: "All media"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Captures.folderFilter === "" && Captures.albumFilter === ""
                             ? Font.DemiBold : Font.Normal
                color: Captures.folderFilter === "" && Captures.albumFilter === ""
                       ? Theme.accent : Theme.foreground
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: allMediaRow.hovered
                       ? root.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: {
                Captures.folderFilter = ""
                Captures.setAlbumFilter("", [])
            }
        }

        MenuItem {
            enabled: false
            height: 24
            contentItem: Text {
                text: "FOLDERS"
                font.family: Theme.fontFamily
                font.pixelSize: 9
                color: root.shade(Theme.foreground, 0.38)
                verticalAlignment: Text.AlignBottom
                leftPadding: 12
            }
        }

        Repeater {
            model: Captures.folders

            MenuItem {
                id: folderRow
                required property int index
                required property string modelData
                height: 30

                contentItem: Text {
                    text: folderRow.modelData
                    elide: Text.ElideMiddle
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Captures.folderFilter === folderRow.modelData
                                 ? Font.DemiBold : Font.Normal
                    color: Captures.folderFilter === folderRow.modelData
                           ? Theme.accent : Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                    rightPadding: 12
                }

                background: Rectangle {
                    color: folderRow.hovered
                           ? root.shade(Theme.foreground, 0.08) : "transparent"
                }
                onTriggered: {
                    Captures.setAlbumFilter("", [])
                    Captures.folderFilter = folderRow.modelData
                }
            }
        }

        MenuItem {
            enabled: false
            height: 24
            contentItem: Text {
                text: "ALBUMS"
                font.family: Theme.fontFamily
                font.pixelSize: 9
                color: root.shade(Theme.foreground, 0.38)
                verticalAlignment: Text.AlignBottom
                leftPadding: 12
            }
        }

        Repeater {
            model: Settings.albumNames

            MenuItem {
                id: albumRow
                required property string modelData
                height: 30
                contentItem: Text {
                    text: albumRow.modelData
                    elide: Text.ElideRight
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Captures.albumFilter === albumRow.modelData
                                 ? Font.DemiBold : Font.Normal
                    color: Captures.albumFilter === albumRow.modelData
                           ? Theme.accent : Theme.foreground
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 12
                    rightPadding: 12
                }
                background: Rectangle {
                    color: albumRow.hovered
                           ? root.shade(Theme.foreground, 0.08) : "transparent"
                }
                onTriggered: {
                    Captures.folderFilter = ""
                    Captures.setAlbumFilter(albumRow.modelData,
                                            Settings.albumPaths(albumRow.modelData))
                }
            }
        }

        MenuItem {
            id: createAlbumRow
            height: 30
            contentItem: Text {
                text: "+ New album"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.accent
                verticalAlignment: Text.AlignVCenter
                leftPadding: 12
            }
            background: Rectangle {
                color: createAlbumRow.hovered
                       ? root.shade(Theme.foreground, 0.08) : "transparent"
            }
            onTriggered: root.createAlbumRequested()
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
