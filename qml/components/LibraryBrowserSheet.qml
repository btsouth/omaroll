import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

FocusScope {
    id: root

    property int section: 0
    property string query: ""
    property string folderMessage: ""
    readonly property bool compact: height < 560
    readonly property var sourceChoices: section === 0 ? Captures.folders : Settings.albumNames
    readonly property var filteredChoices: {
        if (query === "") {
            return sourceChoices
        }
        const folded = query.toLocaleLowerCase()
        const result = []
        for (let index = 0; index < sourceChoices.length; index++) {
            const value = String(sourceChoices[index])
            if (value.toLocaleLowerCase().includes(folded)) {
                result.push(value)
            }
        }
        return result
    }
    signal createAlbumRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function formatCount(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 0)
    }

    function open() {
        section = Captures.albumFilter !== "" ? 1 : 0
        folderSearch.text = ""
        query = ""
        folderMessage = ""
        visible = true
        Qt.callLater(function () {
            root.syncCurrentChoice()
            folderSearch.forceActiveFocus()
        })
    }

    function close() { visible = false }

    function clearLibraryView() {
        Captures.duplicatesOnly = false
        Captures.folderFilter = ""
        Captures.setAlbumFilter("", [])
    }

    function showAll() {
        clearLibraryView()
        close()
    }

    function showDuplicates() {
        Captures.folderFilter = ""
        Captures.setAlbumFilter("", [])
        Captures.duplicatesOnly = true
        close()
    }

    function showFolder(path) {
        Captures.duplicatesOnly = false
        Captures.setAlbumFilter("", [])
        Captures.folderFilter = path
        close()
    }

    function showAlbum(name) {
        Captures.duplicatesOnly = false
        Captures.folderFilter = ""
        Captures.setAlbumFilter(name, Settings.albumPaths(name))
        close()
    }

    function openCurrentChoice() {
        if (!choices.currentItem || !choices.currentItem.visible) {
            return
        }
        section === 0 ? showFolder(choices.currentItem.value)
                      : showAlbum(choices.currentItem.value)
    }

    function nameForPath(path) {
        const parts = path.split("/").filter(function (part) { return part !== "" })
        return parts.length > 0 ? parts[parts.length - 1] : path
    }

    function contextForPath(path) {
        const parts = path.split("/").filter(function (part) { return part !== "" })
        if (parts.length < 2) {
            return path
        }
        return parts.slice(Math.max(0, parts.length - 3), parts.length - 1).join(" / ")
    }

    function firstMatchIndex() {
        return choices.count > 0 ? 0 : -1
    }

    function currentChoiceIndex() {
        const selected = section === 0 ? Captures.folderFilter : Captures.albumFilter
        if (selected !== "") {
            for (let index = 0; index < filteredChoices.length; index++) {
                if (String(filteredChoices[index]) === selected) {
                    return index
                }
            }
        }
        return firstMatchIndex()
    }

    function syncCurrentChoice() {
        choices.currentIndex = currentChoiceIndex()
        if (choices.currentIndex >= 0) {
            choices.positionViewAtIndex(choices.currentIndex, ListView.Center)
        }
    }

    function moveChoice(direction) {
        if (choices.count === 0) {
            return
        }
        choices.currentIndex = (choices.currentIndex + direction + choices.count) % choices.count
        choices.positionViewAtIndex(choices.currentIndex, ListView.Contain)
    }

    function openChoice(index) {
        if (index < 0 || index >= choices.count) {
            return
        }
        const value = String(choices.model[index])
        section === 0 ? showFolder(value) : showAlbum(value)
    }

    visible: false
    anchors.fill: parent
    focus: visible
    onQueryChanged: Qt.callLater(syncCurrentChoice)
    onSectionChanged: Qt.callLater(function () {
        root.syncCurrentChoice()
    })

    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)

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
        width: Math.min(640, root.width - (root.compact ? 28 : 50))
        height: Math.min(680, root.height - (root.compact ? 28 : 50))
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Column {
            anchors.fill: parent
            anchors.margins: root.compact ? 14 : 22
            spacing: root.compact ? 8 : 12

            Row {
                width: parent.width
                spacing: 10

                Text {
                    width: parent.width - closeButton.width - 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Browse library"
                    font.family: Theme.fontFamily
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                }

                PillButton {
                    id: closeButton
                    label: "Close"
                    onClicked: root.close()
                }
            }

            Text {
                width: parent.width
                text: "Choose a source, find a folder, or open an album. Folders are read in place."
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Row {
                spacing: 8

                PillButton {
                    label: "Whole library"
                    active: Captures.folderFilter === "" && Captures.albumFilter === ""
                            && !Captures.duplicatesOnly
                    onClicked: root.showAll()
                }
                PillButton {
                    label: "Exact duplicates"
                    active: Captures.duplicatesOnly
                    onClicked: root.showDuplicates()
                }
                PillButton {
                    label: "+ Add folder"
                    onClicked: folderDialog.open()
                }
            }

            Text {
                text: "SOURCES"
                font.family: Theme.fontFamily
                font.pixelSize: 9
                color: root.shade(Theme.foreground, 0.42)
            }

            Flow {
                width: parent.width
                spacing: 7

                Repeater {
                    model: Library.automaticFolders

                    PillButton {
                        required property var modelData
                        visible: modelData.available
                        label: modelData.label
                        active: Captures.folderFilter === modelData.path
                        onClicked: root.showFolder(modelData.path)
                    }
                }

                Repeater {
                    model: Settings.libraryFolders

                    PillButton {
                        required property string modelData
                        label: root.nameForPath(modelData)
                        active: Captures.folderFilter === modelData
                        onClicked: root.showFolder(modelData)
                    }
                }
            }

            Text {
                width: parent.width
                visible: root.folderMessage !== ""
                text: root.folderMessage
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: Theme.mutedText
            }

            Row {
                spacing: 8

                PillButton {
                    label: "Folders  " + root.formatCount(Captures.folders.length)
                    active: root.section === 0
                    onClicked: {
                        root.section = 0
                        folderSearch.forceActiveFocus()
                    }
                }
                PillButton {
                    label: "Albums  " + root.formatCount(Settings.albumNames.length)
                    active: root.section === 1
                    onClicked: {
                        root.section = 1
                        folderSearch.forceActiveFocus()
                    }
                }
                PillButton {
                    visible: root.section === 1
                    label: "+ New album"
                    onClicked: {
                        root.close()
                        root.createAlbumRequested()
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 34
                radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                color: root.shade(Theme.foreground, 0.06)
                border.width: 1
                border.color: folderSearch.activeFocus
                              ? root.shade(Theme.accent, 0.65)
                              : root.shade(Theme.foreground, 0.16)

                TextInput {
                    id: folderSearch
                    objectName: "librarySearch"
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
                    onTextChanged: root.query = text.trim()
                    Keys.onEscapePressed: root.close()
                    Keys.onDownPressed: {
                        choices.currentIndex = root.firstMatchIndex()
                        choices.forceActiveFocus()
                    }
                    Keys.onReturnPressed: root.openChoice(root.firstMatchIndex())
                    Keys.onEnterPressed: root.openChoice(root.firstMatchIndex())
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    visible: folderSearch.text === ""
                    text: root.section === 0 ? "Find a folder" : "Find an album"
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    color: root.shade(Theme.foreground, 0.35)
                }
            }

            ListView {
                id: choices
                objectName: "libraryChoices"
                width: parent.width
                height: parent.height - y
                clip: true
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.filteredChoices
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                header: Text {
                    width: choices.width
                    height: visible ? 48 : 0
                    visible: root.query !== "" && choices.count === 0
                    text: root.section === 0 ? "No matching folders" : "No matching albums"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                delegate: Rectangle {
                    id: choiceRow
                    required property var modelData
                    readonly property string value: String(modelData)
                    readonly property int itemCount: root.section === 0
                                                     ? Captures.folderItemCount(value)
                                                     : Settings.albumPaths(value).length
                    width: choices.width - 10
                    height: 48
                    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
                    color: rowHover.hovered || ListView.isCurrentItem
                           ? root.shade(Theme.foreground, 0.08) : "transparent"

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 2

                        Text {
                            width: parent.width
                            text: root.section === 0 ? root.nameForPath(choiceRow.value)
                                                     : choiceRow.value
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: (root.section === 0
                                          ? Captures.folderFilter === choiceRow.value
                                          : Captures.albumFilter === choiceRow.value)
                                         ? Font.DemiBold : Font.Normal
                            color: (root.section === 0
                                    ? Captures.folderFilter === choiceRow.value
                                    : Captures.albumFilter === choiceRow.value)
                                   ? Theme.accent : Theme.foreground
                        }

                        Text {
                            width: parent.width
                            text: root.section === 0
                                  ? root.contextForPath(choiceRow.value) + "  ·  "
                                    + root.formatCount(choiceRow.itemCount)
                                    + (choiceRow.itemCount === 1 ? " item" : " items")
                                  : root.formatCount(choiceRow.itemCount)
                                    + (choiceRow.itemCount === 1 ? " item" : " items")
                            elide: Text.ElideMiddle
                            font.family: Theme.fontFamily
                            font.pixelSize: 9
                            color: Theme.mutedText
                        }
                    }

                    HoverHandler { id: rowHover }
                    ToolTip {
                        visible: root.section === 0 && rowHover.hovered
                        text: choiceRow.value
                        delay: 500
                    }
                    TapHandler {
                        onTapped: root.section === 0 ? root.showFolder(choiceRow.value)
                                                     : root.showAlbum(choiceRow.value)
                    }
                }

                Keys.onReturnPressed: root.openCurrentChoice()
                Keys.onEnterPressed: root.openCurrentChoice()
                Keys.onDownPressed: root.moveChoice(1)
                Keys.onUpPressed: root.moveChoice(-1)
                Keys.onEscapePressed: root.close()
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: "Add a folder to Omaroll"
        onAccepted: {
            root.folderMessage = Settings.addLibraryFolder(selectedFolder)
                                 ? "Folder added. Omaroll is scanning it now."
                                 : "That folder is already added, unavailable, or is your home folder."
        }
    }
}
