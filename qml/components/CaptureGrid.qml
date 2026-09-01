import QtQuick
import QtQuick.Controls.Basic

// The library itself: a recycling grid, newest first, with the current day
// tracked as you scroll.
//
// GridView recycles delegates, so the cost of scrolling is set by what is on
// screen rather than by how large the library is.
FocusScope {
    id: root

    property var model: null
    property alias currentIndex: grid.currentIndex
    property alias count: grid.count
    property string currentDayLabel: ""
    property bool layoutReady: true

    // Multi-select is held by path, not by row. Sorting, filtering and rescans
    // all move rows around, and a selection that silently retargets is how bulk
    // delete trashes the wrong files.
    property var checkedSet: ({})
    property int checkedCount: 0

    signal chosen(int index)
    signal deleteRequested(string path)
    signal detailRequested(int index)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function isChecked(path) { return checkedSet[path] === true }

    function toggleChecked(path) {
        if (path === "") {
            return
        }
        const next = Object.assign({}, checkedSet)
        if (next[path]) {
            delete next[path]
        } else {
            next[path] = true
        }
        checkedSet = next
        checkedCount = Object.keys(next).length
    }

    function clearChecked() {
        checkedSet = ({})
        checkedCount = 0
    }

    function checkAll() {
        const next = {}
        for (let i = 0; i < grid.count; i++) {
            next[Captures.pathAt(i)] = true
        }
        checkedSet = next
        checkedCount = Object.keys(next).length
    }

    function checkedPaths() { return Object.keys(checkedSet) }

    // A file deleted outside, or filtered out of view, must not stay counted
    // in "Trash 3". Anything no longer in the visible model is dropped.
    function pruneChecked() {
        if (checkedCount === 0) {
            return
        }
        const next = {}
        for (const path of Object.keys(checkedSet)) {
            if (Captures.rowOf(path) >= 0) {
                next[path] = true
            }
        }
        checkedSet = next
        checkedCount = Object.keys(next).length
    }
    Connections {
        target: Captures
        function onCountChanged() { root.pruneChecked() }
    }

    // Cells stay near a target width and flex to fill the row exactly, so there
    // is never a ragged gutter down the right edge. The scrollbar's width comes
    // out of the available space rather than overlapping the last column.
    readonly property int targetCellWidth: 240
    readonly property int cellSpacing: 12
    readonly property int scrollbarWidth: 12
    readonly property int available: Math.max(1, width - scrollbarWidth)
    readonly property int columns: Math.max(1, Math.floor(available / targetCellWidth))
    readonly property int cellWidth: Math.floor(available / columns)
    onCellWidthChanged: relayoutTimer.restart()

    // GridView resizes existing delegates when cellWidth changes, but does not
    // reliably move them to their new columns. Reattach the same model after a
    // resize settles, preserving the user's place while positions are rebuilt.
    Timer {
        id: relayoutTimer
        interval: 80
        onTriggered: {
            if (grid.count === 0) {
                return
            }
            const selected = grid.currentIndex
            const scroll = grid.contentY
            root.layoutReady = false
            Qt.callLater(function () {
                root.layoutReady = true
                Qt.callLater(function () {
                    grid.currentIndex = Math.min(selected, grid.count - 1)
                    grid.contentY = Math.max(0, Math.min(scroll, grid.contentHeight - grid.height))
                })
            })
        }
    }

    function updateDay() {
        const index = grid.indexAt(grid.contentX + 4, grid.contentY + 4)
        if (index >= 0) {
            root.currentDayLabel = Captures.dayLabelAt(index)
        } else if (grid.count > 0) {
            // indexAt() answers -1 until the view has laid out, which is the
            // state the very first countChanged arrives in. Fall back to the
            // first row rather than leaving the header blank until a scroll.
            root.currentDayLabel = Captures.dayLabelAt(0)
        } else {
            root.currentDayLabel = ""
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        anchors.rightMargin: root.scrollbarWidth
        focus: true
        clip: true

        model: root.layoutReady ? root.model : null

        cellWidth: root.cellWidth
        cellHeight: Math.round(root.cellWidth * 0.68)

        cacheBuffer: cellHeight * 4
        boundsBehavior: Flickable.StopAtBounds
        highlight: null

        // A sort or filter change reshuffles rows; moving rather than snapping
        // makes it read as the same library reordering itself.
        displaced: Transition {
            NumberAnimation { properties: "x,y"; duration: 200; easing.type: Easing.OutQuad }
        }
        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 }
        }

        onContentYChanged: root.updateDay()
        onCountChanged: Qt.callLater(root.updateDay)
        Connections {
            target: Library
            function onDayLabelsChanged() { root.updateDay() }
        }
        onWidthChanged: Qt.callLater(root.updateDay)
        Component.onCompleted: Qt.callLater(root.updateDay)

        delegate: Item {
            id: cell

            required property int index
            required property string path
            required property string fileName
            required property string kindLabel
            required property string timeLabel
            required property string sizeLabel
            required property bool isVideo
            required property bool favorite
            required property bool hidden
            required property double stamp

            width: grid.cellWidth
            height: grid.cellHeight

            CaptureCard {
                anchors.fill: parent
                anchors.margins: Math.round(root.cellSpacing / 2)

                path: cell.path
                fileName: cell.fileName
                kindLabel: cell.kindLabel
                timeLabel: cell.timeLabel
                sizeLabel: cell.sizeLabel
                isVideo: cell.isVideo
                stamp: cell.stamp
                favorite: cell.favorite
                hiddenMark: cell.hidden
                selected: grid.currentIndex === cell.index
                checked: root.isChecked(cell.path)
                selectionMode: root.checkedCount > 0
                // Dragging a checked tile takes the whole selection with it.
                dragPaths: root.isChecked(cell.path) ? root.checkedPaths() : [cell.path]

                onActivated: {
                    grid.currentIndex = cell.index
                    grid.forceActiveFocus()
                }
                onChosen: {
                    grid.currentIndex = cell.index
                    root.detailRequested(cell.index)
                }
                onToggleChecked: root.toggleChecked(cell.path)
            }
        }

        // Arrows are handled by GridView itself; this adds the vim keys and the
        // ones that act on the selection. Single-letter action keys live in
        // Main so they work whether or not the grid has focus.
        Keys.onPressed: function (event) {
            switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (grid.currentIndex >= 0) {
                    root.detailRequested(grid.currentIndex)
                    event.accepted = true
                }
                break
            case Qt.Key_Space:
                if (grid.currentIndex >= 0) {
                    root.chosen(grid.currentIndex)
                    event.accepted = true
                }
                break
            case Qt.Key_Delete:
                if (grid.currentIndex >= 0) {
                    root.deleteRequested(Captures.pathAt(grid.currentIndex))
                    event.accepted = true
                }
                break
            case Qt.Key_X:
                if (grid.currentIndex >= 0) {
                    root.toggleChecked(Captures.pathAt(grid.currentIndex))
                    event.accepted = true
                }
                break
            case Qt.Key_H:
                grid.moveCurrentIndexLeft()
                event.accepted = true
                break
            case Qt.Key_J:
                grid.moveCurrentIndexDown()
                event.accepted = true
                break
            case Qt.Key_K:
                grid.moveCurrentIndexUp()
                event.accepted = true
                break
            case Qt.Key_L:
                grid.moveCurrentIndexRight()
                event.accepted = true
                break
            case Qt.Key_Home:
                grid.currentIndex = 0
                grid.positionViewAtBeginning()
                event.accepted = true
                break
            case Qt.Key_End:
                grid.currentIndex = grid.count - 1
                grid.positionViewAtEnd()
                event.accepted = true
                break
            }
        }
    }

    // Standalone rather than GridView's attached ScrollBar. The attached one is
    // reparented into the Flickable, so it cannot be anchored beside the grid;
    // driving a sibling keeps the bar in its own column where it can never sit
    // on top of a thumbnail.
    ScrollBar {
        id: scrollbar
        orientation: Qt.Vertical
        policy: grid.contentHeight > grid.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff

        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.scrollbarWidth

        size: grid.height / Math.max(grid.contentHeight, 1)
        position: grid.visibleArea.yPosition
        onPositionChanged: {
            if (pressed) {
                grid.contentY = position * grid.contentHeight
            }
        }

        contentItem: Rectangle {
            implicitWidth: scrollbar.hovered || scrollbar.pressed ? 8 : 6
            radius: width / 2
            color: root.shade(Theme.foreground,
                              scrollbar.pressed ? 0.62
                                                : (scrollbar.hovered ? 0.46 : 0.32))
            Behavior on implicitWidth { NumberAnimation { duration: 120 } }
            Behavior on color { ColorAnimation { duration: 140 } }
        }

        background: Rectangle {
            implicitWidth: 8
            radius: width / 2
            color: root.shade(Theme.foreground, scrollbar.hovered ? 0.10 : 0.05)
            Behavior on color { ColorAnimation { duration: 140 } }
        }
    }
}
