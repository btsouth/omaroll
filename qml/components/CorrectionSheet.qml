import QtQuick
import QtQuick.Controls.Basic

// Quick corrections: crop, rotate, flip and resize, saved as a copy.
//
// The preview comes from the edit image provider, so what is on screen is the
// exact frame the writer will produce, minus the crop. The crop is an overlay:
// dragging it never re-composes the picture, it only moves a rectangle whose
// fractions are handed to the writer. The original is never touched.
Item {
    id: root

    property string path: ""
    property string fileName: ""

    property int quarterTurns: 0
    property bool flipHorizontal: false
    property bool flipVertical: false
    // A fine straighten angle in degrees, applied before the crop.
    property real straighten: 0

    // Fractions of the rotated-and-flipped frame, so the crop means the same
    // thing at preview resolution and at full resolution.
    property real cropX: 0
    property real cropY: 0
    property real cropW: 1
    property real cropH: 1
    // Zero means a free crop; otherwise the width/height ratio the crop is
    // held to while it is dragged.
    property real cropAspect: 0
    property string cropAspectLabel: "Free"

    // Zero means "keep the corrected size".
    property int targetWidth: 0
    property int targetHeight: 0

    property size sourceSize: Qt.size(0, 0)
    property bool saving: false
    property string errorText: ""

    readonly property int workingWidth: quarterTurns % 2 === 0 ? sourceSize.width : sourceSize.height
    readonly property int workingHeight: quarterTurns % 2 === 0 ? sourceSize.height : sourceSize.width
    readonly property int croppedWidth: Math.max(1, Math.round(workingWidth * cropW))
    readonly property int croppedHeight: Math.max(1, Math.round(workingHeight * cropH))
    readonly property var previewSource: root.visible && root.path !== "" && !root.saving
        ? "image://edit/" + root.quarterTurns + "." + (root.flipHorizontal ? 1 : 0)
          + "." + (root.flipVertical ? 1 : 0) + "." + Math.round(root.straighten * 10)
          + encodeURIComponent(root.path)
        : ""

    signal saved(string outputPath)
    signal copied()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function open(filePath, name) {
        path = filePath
        fileName = name
        quarterTurns = 0
        flipHorizontal = false
        flipVertical = false
        straighten = 0
        cropX = 0
        cropY = 0
        cropW = 1
        cropH = 1
        targetWidth = 0
        targetHeight = 0
        errorText = ""
        saving = false
        sourceSize = ImageEdit.orientedSize(filePath)
        visible = true
        forceActiveFocus()
        Qt.callLater(function () {
            if (sourceSize.width <= 0 && preview.status === Image.Ready) {
                sourceSize = Qt.size(preview.implicitWidth, preview.implicitHeight)
            }
        })
    }

    function close() {
        visible = false
    }

    function reset() {
        quarterTurns = 0
        flipHorizontal = false
        flipVertical = false
        straighten = 0
        cropX = 0
        cropY = 0
        cropW = 1
        cropH = 1
        cropAspect = 0
        cropAspectLabel = "Free"
        targetWidth = 0
        targetHeight = 0
        errorText = ""
    }

    // Sets the crop to the largest centred rectangle of a ratio. Zero is a
    // free crop, which snaps back to the whole frame.
    function resetCropToAspect(ratio, label) {
        cropAspect = ratio
        cropAspectLabel = label === undefined ? (ratio <= 0 ? "Free" : "") : label
        const pw = preview.paintedWidth
        const ph = preview.paintedHeight
        if (ratio <= 0) {
            cropX = 0
            cropY = 0
            cropW = 1
            cropH = 1
            return
        }
        if (pw <= 0 || ph <= 0) {
            return
        }
        let w = 1
        let h = w * pw / (ratio * ph)
        if (h > 1) {
            h = 1
            w = h * ratio * ph / pw
        }
        cropX = (1 - w) / 2
        cropY = (1 - h) / 2
        cropW = w
        cropH = h
    }

    function rotate(delta) {
        quarterTurns = ((quarterTurns + delta) % 4 + 4) % 4
        errorText = ""
    }

    function setScale(percent) {
        if (percent <= 0) {
            targetWidth = 0
            targetHeight = 0
            return
        }
        targetWidth = Math.max(1, Math.round(croppedWidth * percent / 100))
        targetHeight = Math.max(1, Math.round(croppedHeight * percent / 100))
    }

    function save() {
        if (preview.status !== Image.Ready || saving) {
            return
        }
        saving = true
        errorText = ""
        ImageEdit.saveCopy(root.path, root.quarterTurns, root.flipHorizontal, root.flipVertical,
                           root.straighten, root.cropX, root.cropY, root.cropW, root.cropH,
                           root.targetWidth, root.targetHeight)
    }

    function copyRegion() {
        if (preview.status !== Image.Ready || saving) {
            return
        }
        errorText = ""
        ImageEdit.copyRegion(root.path, root.quarterTurns, root.flipHorizontal,
                             root.flipVertical, root.straighten, root.cropX, root.cropY,
                             root.cropW, root.cropH)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    Connections {
        target: ImageEdit
        function onSaved(outputPath) {
            root.saving = false
            root.saved(outputPath)
            root.close()
        }
        function onCopied() {
            root.copied()
        }
        function onFailed(message) {
            root.saving = false
            root.errorText = message
        }
    }

    // Moving the slider breaks its own value binding, so keep it in step when
    // the angle changes another way, such as Reset.
    Connections {
        target: root
        function onStraightenChanged() { straightenSlider.value = root.straighten }
    }

    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.68)
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
            onClicked: function (mouse) {
                if (mouse.button === Qt.LeftButton && !root.saving) {
                    root.close()
                }
            }
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(1080, root.width - 40)
        height: Math.min(820, root.height - 40)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.97)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Item {
            id: head
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 54

            Text {
                id: title
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "Crop, rotate, resize"
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }

            Text {
                anchors.left: title.right
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideMiddle
                text: root.fileName
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
            anchors.margins: 16
            color: root.shade(Theme.darkerBackground, 0.6)
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            clip: true

            Image {
                id: preview
                objectName: "correctionPreview"
                anchors.fill: parent
                anchors.margins: 12
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                cache: false
                sourceSize: Qt.size(Math.max(320, Math.round(width * Screen.devicePixelRatio)),
                                    Math.max(320, Math.round(height * Screen.devicePixelRatio)))
                source: root.previewSource
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 140 } }
                onStatusChanged: {
                    if (status === Image.Ready && root.sourceSize.width <= 0) {
                        root.sourceSize = Qt.size(implicitWidth, implicitHeight)
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: preview.status !== Image.Ready && !root.saving
                text: preview.status === Image.Error ? "Could not read this file"
                                                     : (root.saving ? "Saving…" : "Loading…")
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: preview.status === Image.Error ? Theme.red : Theme.mutedText
            }

            // The crop overlay. Its coordinates are in the preview's content
            // rectangle; the fractions are what the writer receives.
            Item {
                id: cropLayer
                anchors.fill: preview
                visible: preview.status === Image.Ready && !root.saving

                readonly property real contentX: (width - preview.paintedWidth) / 2
                readonly property real contentY: (height - preview.paintedHeight) / 2
                readonly property real cropLeft: contentX + root.cropX * preview.paintedWidth
                readonly property real cropTop: contentY + root.cropY * preview.paintedHeight
                readonly property real cropRight: cropLeft + root.cropW * preview.paintedWidth
                readonly property real cropBottom: cropTop + root.cropH * preview.paintedHeight

                function setCropPixels(left, top, right, bottom) {
                    const px = preview.paintedWidth
                    const py = preview.paintedHeight
                    if (px <= 0 || py <= 0) {
                        return
                    }
                    const cx = contentX
                    const cy = contentY
                    const minSize = Math.max(16, Math.min(px, py) * 0.05)
                    left = Math.max(cx, Math.min(left, right - minSize))
                    top = Math.max(cy, Math.min(top, bottom - minSize))
                    right = Math.min(cx + px, Math.max(right, left + minSize))
                    bottom = Math.min(cy + py, Math.max(bottom, top + minSize))
                    root.cropX = (left - cx) / px
                    root.cropY = (top - cy) / py
                    root.cropW = (right - left) / px
                    root.cropH = (bottom - top) / py
                    // Hold a chosen aspect: derive the height from the width,
                    // then shrink to stay inside the frame.
                    if (root.cropAspect > 0) {
                        let w = root.cropW
                        let h = w * px / (root.cropAspect * py)
                        if (root.cropY + h > 1) {
                            h = 1 - root.cropY
                            w = h * root.cropAspect * py / px
                        }
                        if (root.cropX + w > 1) {
                            w = 1 - root.cropX
                            h = w * px / (root.cropAspect * py)
                        }
                        root.cropW = w
                        root.cropH = h
                    }
                }

                // Dim everything outside the crop.
                Rectangle {
                    x: cropLayer.contentX
                    y: cropLayer.contentY
                    width: preview.paintedWidth
                    height: Math.max(0, cropLayer.cropTop - cropLayer.contentY)
                    color: Qt.rgba(0, 0, 0, 0.5)
                }
                Rectangle {
                    x: cropLayer.contentX
                    y: cropLayer.cropBottom
                    width: preview.paintedWidth
                    height: Math.max(0, cropLayer.contentY + preview.paintedHeight - cropLayer.cropBottom)
                    color: Qt.rgba(0, 0, 0, 0.5)
                }
                Rectangle {
                    x: cropLayer.contentX
                    y: cropLayer.cropTop
                    width: Math.max(0, cropLayer.cropLeft - cropLayer.contentX)
                    height: Math.max(0, cropLayer.cropBottom - cropLayer.cropTop)
                    color: Qt.rgba(0, 0, 0, 0.5)
                }
                Rectangle {
                    x: cropLayer.cropRight
                    y: cropLayer.cropTop
                    width: Math.max(0, cropLayer.contentX + preview.paintedWidth - cropLayer.cropRight)
                    height: Math.max(0, cropLayer.cropBottom - cropLayer.cropTop)
                    color: Qt.rgba(0, 0, 0, 0.5)
                }

                // The crop frame, draggable as a whole.
                Rectangle {
                    id: cropFrame
                    objectName: "cropFrame"
                    x: cropLayer.cropLeft
                    y: cropLayer.cropTop
                    width: Math.max(1, cropLayer.cropRight - cropLayer.cropLeft)
                    height: Math.max(1, cropLayer.cropBottom - cropLayer.cropTop)
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.accent

                    HoverHandler { cursorShape: Qt.SizeAllCursor }
                    DragHandler {
                        target: null
                        property real startLeft: 0
                        property real startTop: 0
                        property real startW: 1
                        property real startH: 1
                        onActiveChanged: {
                            if (active) {
                                startLeft = cropLayer.cropLeft
                                startTop = cropLayer.cropTop
                                startW = cropLayer.cropRight - cropLayer.cropLeft
                                startH = cropLayer.cropBottom - cropLayer.cropTop
                            }
                        }
                        onTranslationChanged: cropLayer.setCropPixels(
                            startLeft + translation.x, startTop + translation.y,
                            startLeft + startW + translation.x, startTop + startH + translation.y)
                    }
                }

                // Corner handles. Each remembers the edges when the drag began
                // and applies the whole translation to those, so the frame does
                // not accumulate movement as it follows the pointer.
                Rectangle {
                    x: cropLayer.cropLeft - width / 2
                    y: cropLayer.cropTop - height / 2
                    width: 18
                    height: 18
                    radius: 3
                    color: Theme.accent
                    DragHandler {
                        target: null
                        property real sl: 0
                        property real st: 0
                        onActiveChanged: if (active) { sl = cropLayer.cropLeft; st = cropLayer.cropTop }
                        onTranslationChanged: cropLayer.setCropPixels(
                            sl + translation.x, st + translation.y,
                            cropLayer.cropRight, cropLayer.cropBottom)
                    }
                }
                Rectangle {
                    x: cropLayer.cropRight - width / 2
                    y: cropLayer.cropTop - height / 2
                    width: 18
                    height: 18
                    radius: 3
                    color: Theme.accent
                    DragHandler {
                        target: null
                        property real st: 0
                        property real sr: 0
                        onActiveChanged: if (active) { sr = cropLayer.cropRight; st = cropLayer.cropTop }
                        onTranslationChanged: cropLayer.setCropPixels(
                            cropLayer.cropLeft, st + translation.y,
                            sr + translation.x, cropLayer.cropBottom)
                    }
                }
                Rectangle {
                    x: cropLayer.cropLeft - width / 2
                    y: cropLayer.cropBottom - height / 2
                    width: 18
                    height: 18
                    radius: 3
                    color: Theme.accent
                    DragHandler {
                        target: null
                        property real sl: 0
                        property real sb: 0
                        onActiveChanged: if (active) { sl = cropLayer.cropLeft; sb = cropLayer.cropBottom }
                        onTranslationChanged: cropLayer.setCropPixels(
                            sl + translation.x, cropLayer.cropTop,
                            cropLayer.cropRight, sb + translation.y)
                    }
                }
                Rectangle {
                    x: cropLayer.cropRight - width / 2
                    y: cropLayer.cropBottom - height / 2
                    width: 18
                    height: 18
                    radius: 3
                    color: Theme.accent
                    DragHandler {
                        target: null
                        property real sr: 0
                        property real sb: 0
                        onActiveChanged: if (active) { sr = cropLayer.cropRight; sb = cropLayer.cropBottom }
                        onTranslationChanged: cropLayer.setCropPixels(
                            cropLayer.cropLeft, cropLayer.cropTop,
                            sr + translation.x, sb + translation.y)
                    }
                }
            }
        }

        Item {
            id: controls
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: controlFlow.implicitHeight + 30

            Flow {
                id: controlFlow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 6

                PillButton {
                    label: "Rotate left"
                    onClicked: root.rotate(-1)
                }
                PillButton {
                    label: "Rotate right"
                    onClicked: root.rotate(1)
                }
                PillButton {
                    label: "Flip H"
                    active: root.flipHorizontal
                    onClicked: root.flipHorizontal = !root.flipHorizontal
                }
                PillButton {
                    label: "Flip V"
                    active: root.flipVertical
                    onClicked: root.flipVertical = !root.flipVertical
                }
                PillButton {
                    label: "Full frame"
                    onClicked: root.resetCropToAspect(0, "Free")
                }
                PillButton {
                    label: "Reset"
                    onClicked: root.reset()
                }

                Row {
                    width: 210
                    spacing: 6
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "Straighten"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.mutedText
                    }
                    Slider {
                        id: straightenSlider
                        objectName: "straightenSlider"
                        width: 130
                        anchors.verticalCenter: parent.verticalCenter
                        from: -15
                        to: 15
                        stepSize: 0.5
                        value: root.straighten
                        onMoved: root.straighten = value
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 36
                        text: root.straighten.toFixed(1) + "°"
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        color: Theme.foreground
                    }
                }

                Item { width: 12; height: 1 }

                PillButton {
                    label: "Free"
                    active: root.cropAspectLabel === "Free"
                    onClicked: root.resetCropToAspect(0, "Free")
                }
                PillButton {
                    label: "Original"
                    active: root.cropAspectLabel === "Original"
                    onClicked: root.resetCropToAspect(
                                   root.workingHeight > 0
                                   ? root.workingWidth / root.workingHeight : 0, "Original")
                }
                PillButton {
                    label: "1:1"
                    active: root.cropAspectLabel === "1:1"
                    onClicked: root.resetCropToAspect(1, "1:1")
                }
                PillButton {
                    label: "4:3"
                    active: root.cropAspectLabel === "4:3"
                    onClicked: root.resetCropToAspect(4 / 3, "4:3")
                }
                PillButton {
                    label: "3:2"
                    active: root.cropAspectLabel === "3:2"
                    onClicked: root.resetCropToAspect(3 / 2, "3:2")
                }
                PillButton {
                    label: "16:9"
                    active: root.cropAspectLabel === "16:9"
                    onClicked: root.resetCropToAspect(16 / 9, "16:9")
                }

                Item { width: 12; height: 1 }

                PillButton {
                    label: "Original size"
                    active: root.targetWidth === 0 && root.targetHeight === 0
                    onClicked: root.setScale(0)
                }
                PillButton {
                    label: "75%"
                    onClicked: root.setScale(75)
                }
                PillButton {
                    label: "50%"
                    onClicked: root.setScale(50)
                }
                PillButton {
                    label: "25%"
                    onClicked: root.setScale(25)
                }

                Item { width: 12; height: 1 }

                Row {
                    spacing: 6
                    TextField {
                        id: widthField
                        objectName: "correctionWidth"
                        width: 92
                        placeholderText: "W"
                        text: root.targetWidth > 0 ? String(root.targetWidth) : ""
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.brightForeground
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 1; top: 40000 }
                        background: Rectangle {
                            color: root.shade(Theme.darkerBackground, 0.6)
                            radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3
                            border.width: 1
                            border.color: root.shade(Theme.foreground, 0.2)
                        }
                        onEditingFinished: root.targetWidth = text === "" ? 0 : parseInt(text)
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "×"
                        color: Theme.mutedText
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                    }
                    TextField {
                        id: heightField
                        objectName: "correctionHeight"
                        width: 92
                        placeholderText: "H"
                        text: root.targetHeight > 0 ? String(root.targetHeight) : ""
                        font.family: Theme.fontFamily
                        font.pixelSize: 12
                        color: Theme.brightForeground
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator { bottom: 1; top: 40000 }
                        background: Rectangle {
                            color: root.shade(Theme.darkerBackground, 0.6)
                            radius: Theme.cornerRadius > 0 ? Math.min(4, Theme.cornerRadius) : 3
                            border.width: 1
                            border.color: root.shade(Theme.foreground, 0.2)
                        }
                        onEditingFinished: root.targetHeight = text === "" ? 0 : parseInt(text)
                    }
                }

                Text {
                    topPadding: 9
                    text: {
                        const base = root.croppedWidth + " × " + root.croppedHeight
                        if (root.targetWidth > 0 && root.targetHeight > 0) {
                            // The writer keeps the aspect ratio, so this is a
                            // bounding box, not the saved size.
                            return "Fit within " + root.targetWidth + " × " + root.targetHeight
                                   + "  ·  source " + base
                        }
                        if (root.targetWidth > 0 || root.targetHeight > 0) {
                            return "Output " + (root.targetWidth > 0 ? root.targetWidth : "auto")
                                   + " × " + (root.targetHeight > 0 ? root.targetHeight : "auto")
                                   + "  ·  source " + base
                        }
                        return "Output " + base
                    }
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.mutedText
                }
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 8
            spacing: 8

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: root.errorText !== ""
                text: root.errorText
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.red
            }

            PillButton {
                label: "Cancel"
                onClicked: root.close()
            }
            PillButton {
                objectName: "correctionCopyRegion"
                label: "Copy region"
                toolTip: "Copy the cropped region to the clipboard"
                active: !root.saving && preview.status === Image.Ready
                onClicked: root.copyRegion()
            }
            PillButton {
                objectName: "correctionSave"
                label: root.saving ? "Saving…" : "Save a copy"
                active: !root.saving && preview.status === Image.Ready
                onClicked: root.save()
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            if (!root.saving) {
                root.close()
            }
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.save()
            event.accepted = true
        } else if (event.key === Qt.Key_R) {
            root.rotate(event.modifiers & Qt.ShiftModifier ? -1 : 1)
            event.accepted = true
        } else if (event.key === Qt.Key_H) {
            root.flipHorizontal = !root.flipHorizontal
            event.accepted = true
        } else if (event.key === Qt.Key_V) {
            root.flipVertical = !root.flipVertical
            event.accepted = true
        }
    }
}
