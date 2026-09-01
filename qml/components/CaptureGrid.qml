import QtQuick

// The library itself: a recycling grid, newest first, with the current day
// tracked as you scroll.
//
// GridView recycles delegates, so the cost of scrolling is set by what is on
// screen rather than by how large the library is.
FocusScope {
    id: root

    property alias model: grid.model
    property alias currentIndex: grid.currentIndex
    property alias count: grid.count
    property string currentDayLabel: ""

    signal chosen(string path)

    // Cells stay near a target width and flex to fill the row exactly, so there
    // is never a ragged gutter down the right edge.
    readonly property int targetCellWidth: 240
    readonly property int cellSpacing: 12
    readonly property int columns: Math.max(1, Math.floor(grid.width / targetCellWidth))
    readonly property int cellWidth: Math.floor(grid.width / columns)

    function updateDay() {
        const index = grid.indexAt(grid.contentX + 4, grid.contentY + 4)
        root.currentDayLabel = index >= 0 ? Captures.dayLabelAt(index) : ""
    }

    GridView {
        id: grid
        anchors.fill: parent
        focus: true
        clip: true

        cellWidth: root.cellWidth
        cellHeight: Math.round(root.cellWidth * 0.68)

        cacheBuffer: cellHeight * 4
        boundsBehavior: Flickable.StopAtBounds
        highlight: null

        onContentYChanged: root.updateDay()
        onCountChanged: root.updateDay()

        delegate: Item {
            id: cell

            required property int index
            required property string path
            required property string fileName
            required property string kindLabel
            required property string timeLabel
            required property string sizeLabel
            required property bool isVideo

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
                selected: grid.currentIndex === cell.index

                onActivated: {
                    grid.currentIndex = cell.index
                    grid.forceActiveFocus()
                }
                onChosen: root.chosen(cell.path)
            }
        }

        // Arrows are handled by GridView itself; this adds the vim keys and the
        // ones that act on the selection.
        Keys.onPressed: function (event) {
            switch (event.key) {
            case Qt.Key_Return:
            case Qt.Key_Enter:
                if (grid.currentIndex >= 0) {
                    root.chosen(Captures.pathAt(grid.currentIndex))
                    event.accepted = true
                }
                break
            case Qt.Key_H:
                grid.moveCurrentIndexLeft()
                event.accepted = true
                break
            case Qt.Key_L:
                grid.moveCurrentIndexRight()
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
            case Qt.Key_Home:
                grid.currentIndex = 0
                event.accepted = true
                break
            case Qt.Key_End:
                grid.currentIndex = grid.count - 1
                event.accepted = true
                break
            }
        }
    }
}
