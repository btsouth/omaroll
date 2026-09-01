import QtQuick
import QtQuick.Controls.Basic

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
    signal deleteRequested(string path)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
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

        cellWidth: root.cellWidth
        cellHeight: Math.round(root.cellWidth * 0.68)

        cacheBuffer: cellHeight * 4
        boundsBehavior: Flickable.StopAtBounds
        highlight: null

        // A sort or filter change reshuffles rows; fading rather than snapping
        // makes it read as the same library reordering itself.
        displaced: Transition {
            NumberAnimation { properties: "x,y"; duration: 200; easing.type: Easing.OutQuad }
        }
        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 180 }
        }

        onContentYChanged: root.updateDay()
        onCountChanged: Qt.callLater(root.updateDay)
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

        ScrollBar.vertical: ScrollBar {
            id: scrollbar
            policy: ScrollBar.AsNeeded
            width: root.scrollbarWidth
            // Anchored outside the grid's right margin so it never sits on top
            // of a thumbnail.
            parent: root
            anchors.top: root.top
            anchors.right: root.right
            anchors.bottom: root.bottom

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
            case Qt.Key_Delete:
                if (grid.currentIndex >= 0) {
                    root.deleteRequested(Captures.pathAt(grid.currentIndex))
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
}
