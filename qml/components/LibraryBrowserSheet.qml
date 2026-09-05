import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs

FocusScope {
    id: root

    property int section: 0
    property string query: ""
    property string folderMessage: ""
    readonly property bool compact: height < 560
    readonly property var dateChoices: {
        const rows = []
        for (const month of Captures.dateBuckets) {
            rows.push({type: "month", value: month.key, label: month.label,
                       count: month.count, from: month.from, to: month.to})
            for (const day of Captures.dateDays(month.key)) {
                rows.push({type: "day", value: day.date, label: day.label,
                           count: day.count, from: day.date, to: day.date})
            }
        }
        return rows
    }
    // Cameras first, then lenses, each most used first. Rows carry their
    // type so one list can filter on either field.
    readonly property var cameraChoices: {
        const rows = []
        for (const camera of Captures.cameras) {
            rows.push({type: "camera", value: camera.name, label: camera.name,
                       count: camera.count})
        }
        for (const lens of Captures.lenses) {
            rows.push({type: "lens", value: lens.name, label: lens.name, count: lens.count})
        }
        return rows
    }
    readonly property bool hasCameras: Captures.cameras.length > 0
                                       || Captures.lenses.length > 0
    readonly property var sourceChoices: section === 0 ? Captures.folders
                                         : section === 1 ? Settings.albumNames
                                         : section === 2 ? dateChoices
                                         : section === 3 ? Settings.tagNames
                                         : section === 5 ? cameraChoices
                                         : Settings.smartCollectionNames
    readonly property var filteredChoices: {
        if (query === "") {
            return sourceChoices
        }
        const folded = query.toLocaleLowerCase()
        const result = []
        for (let index = 0; index < sourceChoices.length; index++) {
            const value = sourceChoices[index]
            const label = typeof value === "object" ? String(value.label) : String(value)
            if (label.toLocaleLowerCase().includes(folded)) {
                result.push(value)
            }
        }
        return result
    }
    signal createAlbumRequested()
    signal createTagRequested()
    signal saveSmartCollectionRequested()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function formatCount(value) {
        return Number(value).toLocaleString(Qt.locale(), "f", 0)
    }

    function open() {
        section = Captures.albumFilter !== "" ? 1
                  : Captures.dateFrom !== "" || Captures.modifiedAfter !== "" ? 2
                  : Captures.tagFilter !== "" ? 3
                  : Captures.cameraFilter !== "" || Captures.lensFilter !== "" ? 5
                  : Captures.smartCollectionFilter !== "" ? 4 : 0
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
        Captures.similarOnly = false
        Captures.folderFilter = ""
        Captures.setAlbumFilter("", [])
        Captures.setTagFilter("", [])
        Captures.cameraFilter = ""
        Captures.lensFilter = ""
        Captures.clearDateRange()
        Captures.clearSmartCollection()
    }

    function showAll() {
        clearLibraryView()
        close()
    }

    function showDuplicates() {
        clearLibraryView()
        Captures.duplicatesOnly = true
        close()
    }

    function showSimilar() {
        clearLibraryView()
        Captures.similarOnly = true
        close()
    }

    function showFolder(path) {
        clearLibraryView()
        Captures.folderFilter = path
        close()
    }

    function showAlbum(name) {
        clearLibraryView()
        Captures.setAlbumFilter(name, Settings.albumPaths(name))
        close()
    }

    function showDate(choice) {
        clearLibraryView()
        Captures.setDateRange(choice.from, choice.to, 0)
        close()
    }

    function showTag(name) {
        clearLibraryView()
        Captures.setTagFilter(name, Settings.tagPaths(name))
        close()
    }

    function showCamera(choice) {
        clearLibraryView()
        if (choice.type === "lens") {
            Captures.lensFilter = String(choice.value)
        } else {
            Captures.cameraFilter = String(choice.value)
        }
        close()
    }

    function showSmart(name) {
        clearLibraryView()
        const view = Settings.smartCollection(name)
        Captures.applyView(name, view,
                           view.tag ? Settings.tagPaths(String(view.tag)) : [])
        close()
    }

    function isoDate(date) {
        const pad = function (value) { return String(value).padStart(2, "0") }
        return date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate())
    }

    function showRecent(days, modified) {
        clearLibraryView()
        const today = new Date()
        const first = new Date(today.getFullYear(), today.getMonth(), today.getDate() - days + 1)
        Captures.setDateRange(isoDate(first), isoDate(today), modified ? 1 : 0)
        close()
    }

    function showSinceLastVisit() {
        if (Settings.previousVisit === "") {
            showRecent(1, true)
            return
        }
        clearLibraryView()
        Captures.setModifiedAfter(Settings.previousVisit)
        close()
    }

    function openCurrentChoice() {
        if (!choices.currentItem || !choices.currentItem.visible) {
            return
        }
        root.openChoice(choices.currentIndex)
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
        const selected = section === 0 ? Captures.folderFilter
                         : section === 1 ? Captures.albumFilter
                         : section === 3 ? Captures.tagFilter
                         : section === 4 ? Captures.smartCollectionFilter
                         : section === 5 ? (Captures.cameraFilter !== ""
                                            ? Captures.cameraFilter : Captures.lensFilter) : ""
        if (selected !== "") {
            for (let index = 0; index < filteredChoices.length; index++) {
                const choice = filteredChoices[index]
                const value = typeof choice === "object" ? String(choice.value) : String(choice)
                if (value === selected) {
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
        const choice = choices.model[index]
        if (section === 0) showFolder(String(choice))
        else if (section === 1) showAlbum(String(choice))
        else if (section === 2) showDate(choice)
        else if (section === 3) showTag(String(choice))
        else if (section === 5) showCamera(choice)
        else showSmart(String(choice))
    }

    function deleteCurrentChoice() {
        const index = choices.currentIndex
        if (index < 0 || index >= choices.count || section === 0 || section === 2
                || section === 5) {
            return
        }
        const name = String(choices.model[index])
        if (section === 1) Settings.deleteAlbum(name)
        else if (section === 3) Settings.deleteTag(name)
        else Settings.deleteSmartCollection(name)
        folderMessage = "Deleted " + name
        Qt.callLater(syncCurrentChoice)
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
                text: root.hasCameras
                      ? "Browse folders, dates, albums, tags, cameras, saved views, and review sets."
                      : "Browse folders, dates, albums, tags, saved views, and review sets."
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
                            && Captures.tagFilter === "" && Captures.dateFrom === ""
                            && Captures.dateTo === "" && Captures.modifiedAfter === ""
                            && Captures.cameraFilter === "" && Captures.lensFilter === ""
                            && Captures.smartCollectionFilter === ""
                            && !Captures.duplicatesOnly && !Captures.similarOnly
                    onClicked: root.showAll()
                }
                PillButton {
                    label: "Exact duplicates"
                    active: Captures.duplicatesOnly
                    onClicked: root.showDuplicates()
                }
                PillButton {
                    label: "Similar pictures"
                    active: Captures.similarOnly
                    onClicked: root.showSimilar()
                }
                PillButton {
                    label: "+ Add folder"
                    onClicked: folderDialog.open()
                }
            }

            Flow {
                width: parent.width
                spacing: 7

                PillButton { label: "Today"; onClicked: root.showRecent(1, false) }
                PillButton { label: "This week"; onClicked: root.showRecent(7, false) }
                PillButton { label: "New since last visit"; onClicked: root.showSinceLastVisit() }
                PillButton { label: "Recently modified"; onClicked: root.showRecent(7, true) }
                PillButton {
                    label: "Save current view"
                    onClicked: {
                        root.close()
                        root.saveSmartCollectionRequested()
                    }
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

            Flow {
                width: parent.width
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
                    label: "Dates  " + root.formatCount(Captures.dateBuckets.length)
                    active: root.section === 2
                    onClicked: { root.section = 2; folderSearch.forceActiveFocus() }
                }
                PillButton {
                    label: "Tags  " + root.formatCount(Settings.tagNames.length)
                    active: root.section === 3
                    onClicked: { root.section = 3; folderSearch.forceActiveFocus() }
                }
                PillButton {
                    // Only a library with photos from a real camera earns this
                    // section; a screenshot library never sees it.
                    visible: root.hasCameras || root.section === 5
                    label: "Cameras  " + root.formatCount(Captures.cameras.length)
                    active: root.section === 5
                    onClicked: { root.section = 5; folderSearch.forceActiveFocus() }
                }
                PillButton {
                    label: "Smart  " + root.formatCount(Settings.smartCollectionNames.length)
                    active: root.section === 4
                    onClicked: { root.section = 4; folderSearch.forceActiveFocus() }
                }
                PillButton {
                    visible: root.section === 1
                    label: "+ New album"
                    onClicked: {
                        root.close()
                        root.createAlbumRequested()
                    }
                }
                PillButton {
                    visible: root.section === 3
                    label: "+ New tag"
                    onClicked: { root.close(); root.createTagRequested() }
                }
                PillButton {
                    visible: root.section === 4
                    label: "+ Save view"
                    onClicked: { root.close(); root.saveSmartCollectionRequested() }
                }
                PillButton {
                    visible: (root.section === 1 || root.section === 3 || root.section === 4)
                             && choices.currentIndex >= 0
                    label: "Delete selected"
                    toolTip: "Remove this collection. Files are not deleted."
                    onClicked: root.deleteCurrentChoice()
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
                    text: root.section === 0 ? "Find a folder"
                          : root.section === 1 ? "Find an album"
                          : root.section === 2 ? "Find a date"
                          : root.section === 3 ? "Find a tag"
                          : root.section === 5 ? "Find a camera or lens"
                                               : "Find a smart collection"
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
                    text: root.section === 0 ? "No matching folders"
                          : root.section === 1 ? "No matching albums"
                          : root.section === 2 ? "No matching dates"
                          : root.section === 3 ? "No matching tags"
                          : root.section === 5 ? "No matching cameras or lenses"
                                               : "No matching smart collections"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }

                delegate: Rectangle {
                    id: choiceRow
                    required property var modelData
                    required property int index
                    readonly property bool dateChoice: root.section === 2
                    readonly property bool cameraChoice: root.section === 5
                    readonly property bool objectChoice: dateChoice || cameraChoice
                    readonly property string value: objectChoice ? String(modelData.value)
                                                                      : String(modelData)
                    readonly property string choiceLabel: objectChoice ? String(modelData.label)
                                                                            : value
                    readonly property bool selectedChoice:
                        root.section === 0 ? Captures.folderFilter === value
                        : root.section === 1 ? Captures.albumFilter === value
                        : root.section === 2 ? Captures.dateFrom === String(modelData.from)
                                             && Captures.dateTo === String(modelData.to)
                        : root.section === 3 ? Captures.tagFilter === value
                        : root.section === 5 ? (modelData.type === "lens"
                                                ? Captures.lensFilter === value
                                                : Captures.cameraFilter === value)
                                             : Captures.smartCollectionFilter === value
                    readonly property int itemCount: root.section === 0
                                                     ? Captures.folderItemCount(value)
                                                     : root.section === 1
                                                     ? Settings.albumPaths(value).length
                                                     : objectChoice ? Number(modelData.count)
                                                     : root.section === 3
                                                     ? Settings.tagItemCount(value) : 0
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
                                                     : choiceRow.choiceLabel
                            leftPadding: choiceRow.dateChoice && choiceRow.modelData.type === "day"
                                         ? 18 : 0
                            elide: Text.ElideRight
                            font.family: Theme.fontFamily
                            font.pixelSize: 11
                            font.weight: choiceRow.selectedChoice ? Font.DemiBold : Font.Normal
                            color: choiceRow.selectedChoice ? Theme.accent : Theme.foreground
                        }

                        Text {
                            width: parent.width
                            text: root.section === 0
                                  ? root.contextForPath(choiceRow.value) + "  ·  "
                                    + root.formatCount(choiceRow.itemCount)
                                    + (choiceRow.itemCount === 1 ? " item" : " items")
                                  : root.section === 4 ? "Dynamic saved view"
                                  : (choiceRow.cameraChoice
                                     ? (choiceRow.modelData.type === "lens" ? "Lens  ·  "
                                                                            : "Camera  ·  ")
                                     : "")
                                    + root.formatCount(choiceRow.itemCount)
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
                        onTapped: root.openChoice(choiceRow.index)
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
