import QtQuick

// Side-by-side comparison with synchronized zoom and pan.
//
// Every picture shares one zoom factor and one pan offset, so scrolling the
// wheel or dragging the backdrop inspects the same region of each at once.
// This is for telling near-identical pictures apart. Under each picture are
// the facts for choosing a keeper, with the largest marked; "Keep this" moves
// the others to Trash after a confirmation, as one step Undo puts back.
Item {
    id: root

    property var paths: []
    property real zoom: 1.0
    property real panX: 0
    property real panY: 0
    // Per path: {width, height, bytes, size, date, folder}, read when opened.
    property var facts: ({})
    property string largestPath: ""
    property var pendingKeep: ({})
    property var pendingTrash: []

    readonly property int imageCount: paths.length

    // Every picture has loaded; --render waits for this before it grabs.
    function imagesReady() {
        if (!root.visible || root.imageCount === 0 || imageRepeater.count !== root.imageCount) return false
        for (let index = 0; index < imageRepeater.count; ++index) {
            const cell = imageRepeater.itemAt(index)
            if (!cell || !cell.loaded) return false
        }
        return true
    }
    // Under each picture, so the picture's own centre sits this much higher
    // than its cell's.
    readonly property int factsHeight: 56

    // A keep-one cleanup finished: what to report and its undo step, if any.
    signal finished(string message, int undoId)

    function open(list) {
        paths = list === undefined ? [] : list
        zoom = 1.0
        panX = 0
        panY = 0
        readFacts()
        visible = true
        forceActiveFocus()
    }

    function readFacts() {
        const next = {}
        let best = ""
        let bestPixels = -1
        let bestBytes = -1
        let tied = false
        for (const path of paths) {
            const row = Captures.rowOf(path)
            const tile = row >= 0 ? Captures.tileAt(row) : ({})
            const size = Library.pictureSize(path)
            const pixels = size.width > 0 ? size.width * size.height : 0
            const bytes = tile.bytes || 0
            next[path] = {
                width: size.width, height: size.height, bytes: bytes,
                size: tile.sizeLabel || "",
                date: tile.dayLabel ? tile.dayLabel + (tile.timeLabel ? " " + tile.timeLabel : "") : "",
                folder: path.substring(0, path.lastIndexOf("/")) || "/"
            }
            if (pixels > bestPixels || (pixels === bestPixels && bytes > bestBytes)) {
                best = path
                bestPixels = pixels
                bestBytes = bytes
                tied = false
            } else if (pixels === bestPixels && bytes === bestBytes) {
                tied = true
            }
        }
        facts = next
        // Only a real difference is worth pointing at.
        largestPath = paths.length > 1 && !tied ? best : ""
    }

    function factLine(path) {
        const fact = facts[path]
        if (fact === undefined) return ""
        const parts = []
        if (fact.width > 0) parts.push(fact.width + " × " + fact.height)
        if (fact.size !== "") parts.push(fact.size)
        if (fact.date !== "") parts.push(fact.date)
        return parts.join("  ·  ")
    }

    function captureTarget(path) {
        const target = Actions.captureFileAction(Captures.deletionPathAt(Captures.rowOf(path)), path)
        if (target.path !== undefined) target.viewedPath = path
        return target
    }

    // Asks before moving every other compared picture to Trash. The files are
    // captured now, so a rescan behind the prompt cannot change its targets.
    function requestKeep(path) {
        const others = paths.filter(function (other) { return other !== path })
        if (others.length === 0) return
        const keep = captureTarget(path)
        if (keep.path === undefined) return
        const targets = []
        for (const other of others) {
            const target = captureTarget(other)
            if (target.path === undefined) return
            targets.push(target)
        }
        pendingKeep = keep
        pendingTrash = targets
        const name = path.substring(path.lastIndexOf("/") + 1)
        keepConfirm.title = "Keep " + name + " and move "
                + (others.length === 1 ? "the other picture" : "the other " + others.length)
                + " to Trash?"
        const names = others.slice(0, 4).map(function (other) {
            return other.substring(other.lastIndexOf("/") + 1)
        })
        keepConfirm.detail = names.join("\n")
                + (others.length > 4 ? "\nand " + (others.length - 4) + " more" : "")
                + "\n\nThey stay recoverable from Trash, and Undo puts them back."
        keepConfirm.open()
    }

    function keepConfirmed() {
        const keep = pendingKeep
        const targets = pendingTrash
        pendingKeep = ({})
        pendingTrash = []
        if (keep.path === undefined) return
        if (!Actions.capturedStillMatches(keep)) {
            root.finished("The picture to keep changed, so nothing was moved", 0)
            return
        }
        // Let go of the pictures before they move, so nothing reloads them.
        root.close()
        root.paths = []
        // The moves run on a worker, which skips any target that changed and
        // the kept file itself if it was reached by another path.
        keepWork.pending = true
        Actions.keepOneMoveOthersToTrash(keep, targets)
    }

    function close() { visible = false }

    function reset() {
        zoom = 1.0
        panX = 0
        panY = 0
    }

    function zoomBy(factor, x, y) {
        const px = x === undefined ? 0 : x
        const py = y === undefined ? 0 : y
        const boxX = (px - root.panX) / root.zoom
        const boxY = (py - root.panY) / root.zoom
        zoom = Math.max(0.1, Math.min(16, zoom * factor))
        panX = px - boxX * zoom
        panY = py - boxY * zoom
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.82)
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
        width: Math.min(1500, root.width - 24)
        height: Math.min(1000, root.height - 24)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: false
        }

        Item {
            id: head
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 50

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "Compare"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 100
                anchors.verticalCenter: parent.verticalCenter
                text: root.imageCount + (root.imageCount === 1 ? " picture" : " pictures")
                      + "  ·  " + Math.round(root.zoom * 100) + "%"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }
        }

        Rectangle {
            id: stage
            anchors.top: head.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: controls.top
            anchors.margins: 14
            color: root.shade(Theme.darkerBackground, 0.7)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            clip: true

            function zoomAt(factor, scenePosition) {
                const at = grid.mapFromItem(null, scenePosition)
                const column = Math.max(0, Math.min(grid.columns - 1, Math.floor(at.x / (grid.cellWidth + grid.spacing))))
                const row = Math.max(0, Math.min(grid.rows - 1, Math.floor(at.y / (grid.cellHeight + grid.spacing))))
                root.zoomBy(factor, at.x - column * (grid.cellWidth + grid.spacing) - grid.cellWidth / 2,
                                    at.y - row * (grid.cellHeight + grid.spacing)
                                    - (grid.cellHeight - root.factsHeight) / 2)
            }

            WheelHandler {
                target: null
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: function(event) {
                    const delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.pixelDelta.y
                    if (delta !== 0) stage.zoomAt(Math.pow(1.15, delta / 120), point.scenePosition)
                    event.accepted = true
                }
            }
            PinchHandler {
                target: null
                property real last: 1
                onActiveChanged: last = 1
                onActiveScaleChanged: {
                    if (!active || last <= 0) return
                    stage.zoomAt(activeScale / last, centroid.scenePosition)
                    last = activeScale
                }
            }

            // Drag anywhere on the stage to pan every picture together.
            DragHandler {
                target: null
                property real startX: 0
                property real startY: 0
                onActiveChanged: if (active) { startX = root.panX; startY = root.panY }
                onTranslationChanged: {
                    root.panX = startX + translation.x
                    root.panY = startY + translation.y
                }
            }
            TapHandler {
                onDoubleTapped: root.reset()
            }

            Grid {
                id: grid
                anchors.fill: parent
                anchors.margins: 8
                columns: root.imageCount <= 2 ? Math.max(1, root.imageCount) : 2
                spacing: 8

                readonly property int rows: Math.max(1, Math.ceil(root.imageCount / columns))
                readonly property real cellWidth: (width - (columns - 1) * spacing) / columns
                readonly property real cellHeight: (height - (rows - 1) * spacing) / rows

                Repeater {
                    id: imageRepeater
                    objectName: "compareRepeater"
                    model: root.paths

                    Rectangle {
                        id: cell
                        required property string modelData
                        width: grid.cellWidth
                        height: grid.cellHeight
                        color: root.shade(Theme.darkerBackground, 0.5)
                        radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3
                        clip: true

                        // How the picture loads, as in the preview: a very
                        // large one from a copy the size of the screen, with
                        // its detail only once zoomed past that copy.
                        property var picture: ({})
                        readonly property bool loaded: compareImage.status === Image.Ready
                        Component.onCompleted: Library.readPicture(cell.modelData)
                        Connections {
                            target: Library
                            function onPictureRead(path, picture) {
                                if (path === cell.modelData) cell.picture = picture
                            }
                        }

                        Image {
                            id: compareImage
                            objectName: "compareImage"
                            anchors.fill: parent
                            anchors.margins: 4
                            anchors.bottomMargin: root.factsHeight + 4
                            source: cell.picture.path === cell.modelData ? cell.picture.source : ""
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            colorSpace.namedColorSpace: ColorSpace.SRgb
                            smooth: true
                            mipmap: true
                            scale: root.zoom
                            transformOrigin: Item.Center
                            // Keep the pan in screen pixels at every zoom level.
                            transform: Translate {
                                x: root.panX
                                y: root.panY
                            }

                            // The fitted picture itself, inside the cell's margins.
                            Item {
                                id: painted
                                anchors.centerIn: parent
                                width: compareImage.paintedWidth
                                height: compareImage.paintedHeight
                                readonly property bool enlarged: cell.picture.reduced === true
                                    && compareImage.status === Image.Ready
                                    && width * root.zoom * Screen.devicePixelRatio
                                       > compareImage.implicitWidth * 1.05

                                Image {
                                    objectName: "compareDetail"
                                    anchors.fill: parent
                                    source: painted.enlarged && cell.picture.detail !== undefined
                                            ? cell.picture.detail : ""
                                    asynchronous: true
                                    colorSpace.namedColorSpace: ColorSpace.SRgb
                                    autoTransform: true
                                    smooth: true
                                    fillMode: Image.Stretch
                                    visible: status === Image.Ready
                                }

                                PictureArea {
                                    objectName: "compareArea"
                                    viewport: cell
                                    wanted: root.visible && painted.enlarged
                                            && cell.picture.areas === true
                                    pictureScale: cell.picture.width > 0
                                                  ? painted.width / cell.picture.width : 0
                                    // Device pixels per unit here, with the zoom.
                                    devicePixelRatio: Screen.devicePixelRatio * root.zoom
                                    fullSize: Qt.size(cell.picture.width || 0,
                                                      cell.picture.height || 0)
                                    path: cell.modelData
                                    urlFor: (rect, size) => Library.areaUrl(
                                                cell.modelData, cell.picture.version, rect, size)
                                    viewState: [root.zoom, root.panX, root.panY]
                                    smooth: true
                                }
                            }
                        }

                        // What tells copies apart: size, file size, date and
                        // folder, with the largest marked.
                        Rectangle {
                            id: factsBar
                            objectName: "compareFacts"
                            readonly property bool largest: root.largestPath === cell.modelData
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: root.factsHeight
                            color: root.shade(Theme.background, 0.94)
                            border.width: largest ? 1 : 0
                            border.color: Theme.accent
                            radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3

                            Column {
                                id: factsColumn
                                anchors.left: parent.left
                                anchors.right: keepButton.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: 8
                                anchors.rightMargin: 8
                                spacing: 1

                                Row {
                                    width: parent.width
                                    spacing: 6
                                    Rectangle {
                                        visible: factsBar.largest
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: largestLabel.implicitWidth + 10
                                        height: largestLabel.implicitHeight + 2
                                        radius: height / 2
                                        color: Theme.accent
                                        Text {
                                            id: largestLabel
                                            objectName: "compareLargest"
                                            anchors.centerIn: parent
                                            text: "Largest"
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            color: Theme.background
                                        }
                                    }
                                    Text {
                                        width: parent.width - (factsBar.largest ? largestLabel.implicitWidth + 16 : 0)
                                        elide: Text.ElideMiddle
                                        text: cell.modelData.substring(cell.modelData.lastIndexOf("/") + 1)
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: Theme.brightForeground
                                    }
                                }
                                Text {
                                    objectName: "compareFactLine"
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: root.factLine(cell.modelData)
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    color: Theme.foreground
                                }
                                Text {
                                    width: parent.width
                                    elide: Text.ElideMiddle
                                    text: root.facts[cell.modelData] !== undefined
                                          ? root.facts[cell.modelData].folder : ""
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 11
                                    color: Theme.mutedText
                                }
                            }

                            PillButton {
                                id: keepButton
                                objectName: "compareKeep"
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.imageCount > 1
                                width: visible ? implicitWidth : 0
                                label: "Keep this"
                                accessibleName: "Keep " + cell.modelData.substring(cell.modelData.lastIndexOf("/") + 1)
                                toolTip: "Keep this one and move the others to Trash"
                                onClicked: root.requestKeep(cell.modelData)
                            }
                        }
                    }
                }
            }
        }

        // The same dark icon bar as the viewer and the preview, with only
        // the zoom group: every picture shares one zoom.
        ViewerToolbar {
            id: controls
            namePrefix: "compare"
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 12
            width: implicitWidth
            height: implicitHeight
            stepControls: false
            transformControls: false
            trashControl: false
            actualControl: false
            fitted: root.zoom === 1 && root.panX === 0 && root.panY === 0
            onZoomOut: root.zoomBy(1 / 1.25)
            onZoomIn: root.zoomBy(1.25)
            onToggleFit: root.reset()
        }

        PillButton {
            objectName: "compareDone"
            anchors.verticalCenter: controls.verticalCenter
            anchors.right: parent.right
            anchors.rightMargin: 16
            label: "Done"
            active: true
            onClicked: root.close()
        }
    }

    Connections {
        id: keepWork
        property bool pending: false
        target: Actions
        enabled: pending
        function onKeepOneFinished(moved, skipped, trashed) {
            keepWork.pending = false
            // The whole cleanup is one step, so one Undo puts it all back.
            const undoId = Settings.pushTrashUndo(trashed, null)
            const total = moved + skipped.length
            root.finished(skipped.length === 0
                          ? "Kept one, moved " + moved + (moved === 1 ? " picture" : " pictures") + " to Trash"
                          : "Moved " + moved + " of " + total + " pictures to Trash", undoId)
        }
    }

    ConfirmSheet {
        id: keepConfirm
        objectName: "compareKeepConfirm"
        confirmLabel: "Move to Trash"
        onAccepted: root.keepConfirmed()
        onVisibleChanged: if (!visible && root.visible) root.forceActiveFocus()
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        } else if (event.key === Qt.Key_0) {
            root.reset()
            event.accepted = true
        } else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            root.zoomBy(1.25)
            event.accepted = true
        } else if (event.key === Qt.Key_Minus) {
            root.zoomBy(1 / 1.25)
            event.accepted = true
        }
    }
}
