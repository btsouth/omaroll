import QtQuick

// Drag over a rendered PDF page to select the words under the pointer.
//
// It lives inside the page's own Image, so its coordinates are the image's
// coordinates: zoom, pan, page rotation and letterboxing are handled by the
// parent, and a normalized point is just the local coordinate divided by the
// item's size. The words come from PdfInfo, which reads them a page at a time
// and answers with one normalized rectangle per selected line.
Item {
    id: root

    // The 1-based page this layer covers.
    property int page: 1
    // Whether a drag selects text. While it is false the layer is inert and
    // the surface underneath keeps its own drag (pan, scroll).
    property bool selecting: false

    readonly property var selectedRects: PdfInfo.selectionPage === root.page
                                         ? PdfInfo.selectionRects : []

    // The rectangle being dragged right now, in local coordinates.
    property real fromX: 0
    property real fromY: 0
    property real toX: 0
    property real toY: 0
    property bool dragging: false
    // Kept after a release until the words arrive, so the first drag on a page
    // does not blink from the marquee to the highlight.
    property bool pending: false

    function ratio(value, span) {
        return span > 0 ? Math.max(0, Math.min(1, value / span)) : 0
    }

    // The words already selected on this page.
    Repeater {
        model: root.selectedRects

        delegate: Rectangle {
            required property var modelData
            x: modelData.x * root.width
            y: modelData.y * root.height
            width: modelData.width * root.width
            height: modelData.height * root.height
            radius: 1
            color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.28)
        }
    }

    // The rectangle being dragged now.
    Rectangle {
        objectName: "pdfSelectionMarquee"
        visible: root.dragging || root.pending
        x: Math.min(root.fromX, root.toX)
        y: Math.min(root.fromY, root.toY)
        width: Math.abs(root.toX - root.fromX)
        height: Math.abs(root.toY - root.fromY)
        color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.16)
        border.width: 1
        border.color: Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.55)
    }

    Connections {
        target: PdfInfo
        // The answer arrived, whatever it was: the marquee has served its
        // purpose and the highlight takes over.
        function onSelectionChanged() {
            root.pending = false
        }
        function onSelectionFailed(message) {
            root.pending = false
        }
    }

    MouseArea {
        id: drag
        anchors.fill: parent
        enabled: root.selecting
        acceptedButtons: Qt.LeftButton
        cursorShape: root.selecting ? Qt.IBeamCursor : Qt.ArrowCursor
        // Keep this drag: the page underneath must not pan or flick away while
        // the reader is selecting across it, and the wheel still scrolls the
        // document because the surface itself stays live.
        preventStealing: true

        property bool moved: false

        onPressed: function(mouse) {
            root.fromX = mouse.x
            root.fromY = mouse.y
            root.toX = mouse.x
            root.toY = mouse.y
            drag.moved = false
            root.dragging = false
            root.pending = false
            // A press starts a new selection, so the old one goes away instead
            // of staying lit underneath the new drag.
            PdfInfo.clearSelection()
        }

        onPositionChanged: function(mouse) {
            if (!drag.moved
                    && Math.abs(mouse.x - root.fromX) < 3
                    && Math.abs(mouse.y - root.fromY) < 3) {
                return
            }
            drag.moved = true
            root.dragging = true
            root.toX = mouse.x
            root.toY = mouse.y
        }

        onReleased: function(mouse) {
            if (!drag.moved) {
                // A press that never moved is a click, not a selection.
                root.dragging = false
                return
            }
            root.toX = mouse.x
            root.toY = mouse.y
            root.dragging = false
            root.pending = true
            PdfInfo.updateSelection(root.page,
                                    root.ratio(root.fromX, root.width),
                                    root.ratio(root.fromY, root.height),
                                    root.ratio(root.toX, root.width),
                                    root.ratio(root.toY, root.height))
        }
    }
}
