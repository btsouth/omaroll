import QtQuick
import QtQuick.Controls.Basic

// Similar pictures, one section per set: a header naming the set and its size,
// then its tiles in rows. A GridView has no sections, and the review filter
// keeps each set's rows together, so the sections are laid out here from the
// proxy's set boundaries while selection, marks and actions stay with the
// grid that hosts this view.
FocusScope {
    id: root

    // The CaptureGrid this view stands in for.
    property Item host: null
    property int columns: 1
    property int cellWidth: 100
    property int cellHeight: 68
    property int cellSpacing: 12
    property var rows: []
    property int revision: 0
    readonly property int setCount: {
        let count = 0
        for (const row of rows) if (row.header) count++
        return count
    }

    function rebuild() {
        const sections = root.visible ? Captures.reviewSections() : []
        const next = []
        for (const section of sections) {
            next.push({header: true, first: section.first, count: section.count,
                       label: section.label})
            for (let offset = 0; offset < section.count; offset += root.columns) {
                next.push({header: false, first: section.first + offset,
                           count: Math.min(root.columns, section.count - offset)})
            }
        }
        rows = next
        root.revision++
        Qt.callLater(root.revealCurrent)
    }

    function scheduleRebuild() { rebuildTimer.restart() }

    function listRowOf(index) {
        for (let i = 0; i < rows.length; ++i) {
            const row = rows[i]
            if (!row.header && index >= row.first && index < row.first + row.count) return i
        }
        return -1
    }

    function setPaths(first, count) {
        const paths = []
        for (let i = first; i < first + count; ++i) paths.push(Captures.pathAt(i))
        return paths
    }

    function revealCurrent() {
        if (!root.host || root.host.currentIndex < 0) return
        const row = root.listRowOf(root.host.currentIndex)
        if (row < 0) return
        // The first row of a set brings its header into view with it.
        const before = row > 0 && rows[row - 1].header ? row - 1 : row
        list.positionViewAtIndex(before, ListView.Contain)
        list.positionViewAtIndex(row, ListView.Contain)
    }

    function select(index) {
        if (!root.host || index < 0 || index >= Captures.count) return
        root.host.selectedPath = Captures.pathAt(index)
        root.host.currentIndex = index
        root.revealCurrent()
    }

    function moveVertically(step) {
        const current = root.host ? root.host.currentIndex : -1
        const row = root.listRowOf(current)
        if (row < 0) {
            root.select(0)
            return
        }
        const column = current - rows[row].first
        for (let i = row + step; i >= 0 && i < rows.length; i += step) {
            if (!rows[i].header) {
                root.select(rows[i].first + Math.min(column, rows[i].count - 1))
                return
            }
        }
    }

    // Every tile on screen has its thumbnail, as the grid reports for renders.
    function viewportReady() {
        if (!visible || rows.length === 0 || list.height <= 0) return false
        const first = Math.max(0, list.indexAt(4, list.contentY + 4))
        let last = list.indexAt(4, list.contentY + list.height - 4)
        if (last < 0) last = rows.length - 1
        for (let i = first; i <= last; ++i) {
            const item = list.itemAtIndex(i)
            if (!item) return false
            if (!item.presented()) return false
        }
        return true
    }

    Timer {
        id: rebuildTimer
        interval: 0
        onTriggered: root.rebuild()
    }

    onVisibleChanged: scheduleRebuild()
    onColumnsChanged: scheduleRebuild()
    Component.onCompleted: scheduleRebuild()

    Connections {
        target: Captures
        function onCountChanged() { root.scheduleRebuild() }
        function onLayoutChanged() { root.scheduleRebuild() }
        function onModelReset() { root.scheduleRebuild() }
        function onRowsMoved() { root.scheduleRebuild() }
        function onDataChanged() { root.revision++ }
    }
    Connections {
        target: root.host
        function onCurrentIndexChanged() { if (root.visible) root.revealCurrent() }
    }

    // Standalone, beside the list, as the grid's is.
    ScrollBar {
        id: scrollbar
        orientation: Qt.Vertical
        policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 12
        size: list.visibleArea.heightRatio
        position: list.visibleArea.yPosition
        onPositionChanged: {
            if (pressed) list.contentY = list.originY + position * list.contentHeight
        }
    }

    ListView {
        id: list
        objectName: "reviewSetsList"
        anchors.fill: parent
        anchors.rightMargin: 12
        focus: true
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.rows
        reuseItems: false
        cacheBuffer: root.cellHeight * 3

        header: Item {
            width: list.width
            height: 44

            Text {
                id: summary
                anchors.left: parent.left
                anchors.leftMargin: Math.round(root.cellSpacing / 2)
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, parent.width - picker.width - 24)
                elide: Text.ElideRight
                text: root.setCount + (root.setCount === 1 ? " similar set" : " similar sets")
                      + " · review before removing anything"
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.mutedText
            }
            SimilarStrictnessPicker {
                id: picker
                anchors.right: parent.right
                anchors.rightMargin: Math.round(root.cellSpacing / 2)
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        delegate: Item {
            id: rowItem
            required property int index
            required property var modelData
            width: list.width
            height: modelData.header ? 40 : root.cellHeight

            function presented() {
                if (modelData.header) return true
                for (let i = 0; i < tiles.count; ++i) {
                    const tile = tiles.itemAt(i)
                    if (!tile || !tile.presented) return false
                }
                return true
            }

            // Set header.
            Item {
                visible: rowItem.modelData.header
                anchors.fill: parent
                anchors.leftMargin: Math.round(root.cellSpacing / 2)
                anchors.rightMargin: Math.round(root.cellSpacing / 2)

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 4
                    height: 1
                    color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.14)
                }
                Text {
                    id: setTitle
                    objectName: "reviewSetHeader"
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: rowItem.modelData.header ? rowItem.modelData.label : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.brightForeground
                }
                Text {
                    anchors.left: setTitle.right
                    anchors.leftMargin: 8
                    anchors.right: compareButton.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: rowItem.modelData.header
                          ? rowItem.modelData.count + " pictures" : ""
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
                PillButton {
                    id: compareButton
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: rowItem.modelData.header
                    label: "Compare"
                    accessibleName: "Compare " + (rowItem.modelData.label || "")
                    toolTip: "Compare this set side by side and keep one"
                    onClicked: root.host.compareRequested(
                                   root.setPaths(rowItem.modelData.first, rowItem.modelData.count))
                }
            }

            // A row of tiles from one set.
            Row {
                visible: !rowItem.modelData.header
                Repeater {
                    id: tiles
                    model: rowItem.modelData.header ? 0 : rowItem.modelData.count

                    Item {
                        id: cell
                        required property int index
                        readonly property int row: rowItem.modelData.first + index
                        readonly property var tile: { void root.revision; return Captures.tileAt(cell.row) }
                        readonly property bool presented: card.thumbnailPresented
                        readonly property string path: tile.path || ""
                        width: root.cellWidth
                        height: root.cellHeight

                        CaptureCard {
                            id: card
                            anchors.fill: parent
                            anchors.margins: Math.round(root.cellSpacing / 2)

                            path: cell.path
                            fileName: cell.tile.fileName || ""
                            kindLabel: cell.tile.kindLabel || ""
                            timeLabel: cell.tile.timeLabel || ""
                            sizeLabel: cell.tile.sizeLabel || ""
                            isVideo: cell.tile.isVideo === true
                            isDocument: cell.tile.isDocument === true
                            stamp: cell.tile.stamp || 0
                            thumbnailVersion: cell.tile.thumbnailVersion || ""
                            favorite: cell.tile.favorite === true
                            rating: cell.tile.rating || 0
                            hiddenMark: cell.tile.hidden === true
                            ocrSnippet: cell.tile.ocrSnippet || ""
                            caption: cell.tile.caption || ""
                            rawFormat: cell.tile.rawFormat || ""
                            selected: !!root.host && root.host.currentIndex === cell.row
                            keyboardCurrent: selected && list.activeFocus
                            checked: !!root.host && root.host.isChecked(cell.path)
                            selectionMode: !!root.host && root.host.checkedCount > 0
                            dragPaths: !!root.host && root.host.isChecked(cell.path)
                                       ? root.host.checkedPaths() : [cell.path]

                            onActivated: {
                                Captures.commitPendingSearch()
                                root.select(Captures.rowOf(cell.path))
                                list.forceActiveFocus()
                            }
                            onChosen: {
                                Captures.commitPendingSearch()
                                const row = Captures.rowOf(cell.path)
                                if (row < 0) return
                                root.select(row)
                                root.host.detailRequested(row)
                            }
                            onContextRequested: function (x, y) {
                                Captures.commitPendingSearch()
                                const row = Captures.rowOf(cell.path)
                                if (row < 0) return
                                root.select(row)
                                root.host.contextRequested(row, x, y)
                            }
                            onToggleChecked: {
                                Captures.commitPendingSearch()
                                if (Captures.rowOf(cell.path) >= 0) root.host.toggleChecked(cell.path)
                            }
                        }
                    }
                }
            }
        }

        // The grid's keys, moving through the sets' rows rather than the
        // grid's cells.
        Keys.onPressed: function (event) {
            Captures.commitPendingSearch()
            const current = root.host.currentIndex
            switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (current >= 0) root.host.detailRequested(current)
                event.accepted = current >= 0
                break
            case Qt.Key_Space:
                if (current >= 0) root.host.chosen(current)
                event.accepted = current >= 0
                break
            case Qt.Key_Delete:
                if (event.modifiers & Qt.ShiftModifier) {
                    event.accepted = true
                    break
                }
                if (current >= 0) root.host.deleteRequested(Captures.pathAt(current))
                event.accepted = current >= 0
                break
            case Qt.Key_X:
                if (current >= 0) root.host.toggleChecked(Captures.pathAt(current))
                event.accepted = current >= 0
                break
            case Qt.Key_Left:
            case Qt.Key_H:
                root.select(current <= 0 ? Captures.count - 1 : current - 1)
                event.accepted = true
                break
            case Qt.Key_Right:
            case Qt.Key_L:
                root.select(current >= Captures.count - 1 ? 0 : current + 1)
                event.accepted = true
                break
            case Qt.Key_Up:
            case Qt.Key_K:
                root.moveVertically(-1)
                event.accepted = true
                break
            case Qt.Key_Down:
            case Qt.Key_J:
                root.moveVertically(1)
                event.accepted = true
                break
            case Qt.Key_Tab:
                root.host.sectionStepRequested(event.modifiers & Qt.ShiftModifier ? -1 : 1)
                event.accepted = true
                break
            case Qt.Key_Backtab:
                root.host.sectionStepRequested(-1)
                event.accepted = true
                break
            case Qt.Key_Home:
                root.select(0)
                list.positionViewAtBeginning()
                event.accepted = true
                break
            case Qt.Key_End:
                root.select(Captures.count - 1)
                event.accepted = true
                break
            }
        }
    }
}
