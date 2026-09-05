import QtQuick

// One capture. The thumbnail is drawn at full opacity on top of the translucent
// chrome: dimming the pixels someone is trying to look at is a bug, not a style,
// which is why every media app in Omarchy opts out of window transparency.
Item {
    id: root

    property string path: ""
    property string fileName: ""
    property string kindLabel: ""
    property string timeLabel: ""
    property string sizeLabel: ""
    property bool isVideo: false
    property bool isDocument: false
    // mtime, carried in the thumbnail URL so a rewritten file busts Qt's
    // in-memory pixmap cache as well as the disk one.
    property double stamp: 0
    property bool favorite: false
    property bool hiddenMark: false
    property bool selected: false
    property bool checked: false
    property bool selectionMode: false
    property string ocrSnippet: ""
    property bool thumbnailReady: false
    readonly property bool thumbnailPresented: thumbnail.status === Image.Ready
                                               && thumbnail.opacity >= 0.999
    // What leaves when this tile is dragged out. The grid sets it to the whole
    // selection when this tile is part of one.
    property var dragPaths: [path]
    // Raised before the drag image is grabbed, so the badge below is in it.
    property bool dragging: false

    signal activated()
    signal chosen()
    signal toggleChecked()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    // Hovering a recording walks a few frames through the clip, so you can tell
    // two similar recordings apart without opening either. Frames are generated
    // lazily on first hover and cached, never during a scan.
    readonly property var scrubStops: [20, 40, 60, 80]
    property int scrubIndex: 0
    readonly property int scrubPercent: scrubStops[scrubIndex]

    Timer {
        id: scrubTimer
        interval: 700
        repeat: true
        running: root.isVideo && hover.hovered && root.path !== ""
        // Back to the resting frame when the hover ends, or a browsed-over
        // tile parks mid-scrub and no longer matches its neighbours.
        onRunningChanged: if (!running) root.scrubIndex = 0
        onTriggered: root.scrubIndex = (root.scrubIndex + 1) % root.scrubStops.length
    }

    onPathChanged: {
        root.scrubIndex = 0
        root.thumbnailReady = false
    }

    // Drag out and it drops as the real file into anything on the desktop that
    // takes one: a Discord message, a Nautilus window, a browser upload. Copy
    // only. A drop that moved the file would break the promise that omaroll
    // never moves a capture.
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction
    Drag.proposedAction: Qt.CopyAction
    Drag.mimeData: ({
        "text/uri-list": Library.uriList(root.dragPaths),
        "text/plain": root.dragPaths.join("\n")
    })
    Drag.onDragFinished: {
        root.dragging = false
        // The release that ended the drag went to the drop target, not to us,
        // so the handler would otherwise stay active and miss the next drag.
        dragOut.enabled = false
        dragOut.enabled = true
    }

    DragHandler {
        id: dragOut
        target: null
        acceptedButtons: Qt.LeftButton
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        // The GridView is a Flickable that would otherwise steal the drag to
        // scroll. Wheel and scrollbar cover scrolling; a mouse drag on a tile
        // means "take this file".
        grabPermissions: PointerHandler.CanTakeOverFromItems
                         | PointerHandler.CanTakeOverFromHandlersOfDifferentType
                         | PointerHandler.ApprovesTakeOverByHandlersOfSameType
        onActiveChanged: {
            if (!active || root.path === "") {
                return
            }
            // The tile itself is the drag image, grabbed at the pointer's
            // offset so it does not jump under the cursor.
            root.Drag.hotSpot = Qt.point(dragOut.centroid.pressPosition.x,
                                         dragOut.centroid.pressPosition.y)
            root.dragging = true
            root.grabToImage(function (result) {
                // The grab is asynchronous; a press-move-release quicker than
                // it would otherwise start a drag with no button held.
                if (!dragOut.active) {
                    root.dragging = false
                    return
                }
                root.Drag.imageSource = result.url
                root.Drag.active = true
            })
        }
    }

    Rectangle {
        id: frame
        anchors.fill: parent
        color: root.shade(Theme.background, root.selected ? 0.62 : 0.34)
        radius: Theme.cornerRadius
        border.width: 1
        border.color: root.shade(Theme.foreground, hover.hovered ? 0.22 : 0.10)
        clip: true

        Behavior on color { ColorAnimation { duration: 180; easing.type: Easing.OutQuad } }
        Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }

        Image {
            id: thumbnail
            anchors.fill: parent
            anchors.margins: 1
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            retainWhileLoading: true
            // Decode at the size actually drawn, on the ratio of the screen this
            // window is on. Without the ratio, a 1.5x monitor shows an upscaled
            // tile and the whole grid reads as soft.
            sourceSize: Qt.size(Math.round(root.width), Math.round(root.height))
            // "image://thumbs/<ratio>[@<seek%>][~<stamp>]<encoded path>". The
            // path is percent-encoded whole, so a '#', a '?' or a literal '%'
            // in a filename survives URL parsing; the provider decodes once.
            // Nothing is requested before the first layout, or every tile would
            // ask once at the fallback size and again at its real one.
            source: root.path === "" || root.width <= 0
                    ? ""
                    : "image://thumbs/" + Screen.devicePixelRatio
                      + (root.isVideo ? "@" + root.scrubPercent : "")
                      + "~" + root.stamp
                      + encodeURIComponent(root.path)
            smooth: true
            mipmap: true
            opacity: root.thumbnailReady ? 1 : 0

            onStatusChanged: {
                if (status === Image.Ready) {
                    root.thumbnailReady = true
                }
            }

            Behavior on opacity { NumberAnimation { duration: 200; easing.type: Easing.OutQuad } }
        }

        // Placeholder while a thumbnail is being made, and the resting state for
        // anything that cannot produce one.
        Text {
            anchors.centerIn: parent
            visible: !root.thumbnailReady
            text: root.isVideo ? "▶" : (root.isDocument ? "PDF" : "▦")
            font.family: Theme.fontFamily
            font.pixelSize: 22
            color: root.shade(Theme.foreground, 0.28)
        }

        // Hidden entries stay legible but visibly set aside, so "show hidden"
        // does not just look like the filter failed.
        Rectangle {
            anchors.fill: parent
            visible: root.hiddenMark
            color: root.shade(Theme.background, 0.55)
        }

        // A fixed dark strip keeps metadata readable over every image and theme.
        // It is deliberately compact so the picture still owns almost the whole
        // card.
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: root.ocrSnippet !== "" ? 52 : 30
            visible: root.thumbnailReady
            color: Qt.rgba(0, 0, 0, 0.72)
        }

        Text {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: metadataRow.top
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.bottomMargin: 2
            visible: root.ocrSnippet !== "" && root.thumbnailReady
            text: root.ocrSnippet
            elide: Text.ElideRight
            font.family: Theme.fontFamily
            font.pixelSize: 10
            color: "#ffffff"
            opacity: 0.90
        }

        Row {
            id: metadataRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 8
            spacing: 8

            // White over the scrim once the picture is up; theme text over the
            // bare frame before that, or on a light theme it would vanish.
            Text {
                text: root.timeLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.DemiBold
                color: root.thumbnailReady ? "#ffffff" : Theme.foreground
                opacity: 1
            }

            Text {
                text: root.sizeLabel
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: root.thumbnailReady ? "#ffffff" : Theme.foreground
                opacity: 0.82
            }
        }

        // Kind marker. A recording needs to be tellable from a screenshot at a
        // glance, before any text is read.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 8
            visible: root.isVideo
            width: badge.implicitWidth + 12
            height: badge.implicitHeight + 6
            radius: 3
            color: Qt.rgba(0, 0, 0, 0.55)

            Text {
                id: badge
                anchors.centerIn: parent
                text: (scrubTimer.running ? "◉ " : "▶ ") + root.kindLabel
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: "#ffffff"
            }
        }

        Text {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 8
            visible: root.favorite
            text: "★"
            font.pixelSize: 15
            color: Theme.yellow
            style: Text.Outline
            styleColor: Qt.rgba(0, 0, 0, 0.6)
        }

        // Multi-select checkbox. Only present once selection is under way or the
        // card is hovered, so the resting grid stays pictures rather than chrome.
        Rectangle {
            id: checkBox
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 7
            visible: root.selectionMode || hover.hovered
            width: 18
            height: 18
            radius: 3
            color: root.checked ? Theme.accent : Qt.rgba(0, 0, 0, 0.5)
            border.width: 1
            border.color: root.checked ? Theme.accent : Qt.rgba(1, 1, 1, 0.5)

            Text {
                anchors.centerIn: parent
                visible: root.checked
                text: "✓"
                font.pixelSize: 11
                font.weight: Font.Bold
                color: Theme.background
            }
        }

        // Count badge, drawn only while a multi-file drag is being captured, so
        // the drag image says how many files are leaving.
        Rectangle {
            anchors.centerIn: parent
            visible: root.dragging && root.dragPaths.length > 1
            width: dragCount.implicitWidth + 24
            height: dragCount.implicitHeight + 14
            radius: height / 2
            color: Theme.accent

            Text {
                id: dragCount
                anchors.centerIn: parent
                text: root.dragPaths.length + " files"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                font.weight: Font.Bold
                color: Theme.background
            }
        }

        // Keep the current/checked outline above the thumbnail and metadata
        // strip. A Rectangle's own border is painted below its children, which
        // let the bottom strip cover the lower edge of the selection.
        Rectangle {
            anchors.fill: parent
            z: 20
            color: "transparent"
            radius: Theme.cornerRadius
            border.width: root.selected || root.checked ? 2 : 0
            border.color: root.checked ? Theme.accent : root.shade(Theme.accent, 0.75)
        }

        HoverHandler { id: hover }

        TapHandler {
            acceptedButtons: Qt.LeftButton
            onSingleTapped: function (eventPoint) {
                const checkboxPoint = checkBox.mapFromItem(frame, eventPoint.position)
                if (root.selectionMode || checkBox.contains(checkboxPoint)) {
                    root.toggleChecked()
                } else {
                    root.activated()
                }
            }
            onDoubleTapped: root.chosen()
            // Touch has no hover, so no checkbox; a long press selects instead.
            onLongPressed: root.toggleChecked()
        }

        // Right click goes straight to the actions, which is what a right
        // click on a file means everywhere else on the desktop.
        TapHandler {
            acceptedButtons: Qt.RightButton
            onSingleTapped: root.chosen()
        }
    }
}
