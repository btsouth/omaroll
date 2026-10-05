import QtQuick
import QtQuick.Controls.Basic

// One rotate/flip/resize applied to a whole selection, each file written as
// its own copy. No crop and no straighten: those are per-picture decisions,
// which is what the single-picture editor is for.
Item {
    id: root

    property var paths: []
    property int quarterTurns: 0
    property bool flipHorizontal: false
    property bool flipVertical: false
    property int targetWidth: 0
    property int doneCount: 0
    property bool running: false
    property int jobGeneration: -1
    property var failedPaths: []
    property string errorText: ""
    property bool hasAnimation: false

    readonly property int fileCount: paths.length

    signal finished(int succeeded, int failed, var outputs)

    function open(list) {
        jobGeneration = ImageEdit.nextJobId()
        paths = list === undefined ? [] : list
        failedPaths = []
        errorText = ""
        hasAnimation = paths.some(function (path) { return ImageEdit.isAnimated(path) })
        quarterTurns = 0
        flipHorizontal = false
        flipVertical = false
        targetWidth = 0
        doneCount = 0
        running = false
        visible = true
        forceActiveFocus()
    }

    function close() { jobGeneration = -1; visible = false }

    function apply() {
        if (running || ImageEdit.busy || paths.length === 0) {
            return
        }
        running = true
        errorText = ""
        doneCount = 0
        ImageEdit.saveCopies(root.paths, root.quarterTurns, root.flipHorizontal,
                             root.flipVertical, root.targetWidth, 0, root.jobGeneration)
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    Connections {
        target: ImageEdit
        function onBatchProgress(done, total, jobId) {
            if (jobId === root.jobGeneration && root.visible) root.doneCount = done
        }
        function onBatchFinished(succeeded, failed, results, jobId) {
            if (jobId !== root.jobGeneration || !root.visible) return
            root.running = false
            const outputs = []
            const failedPaths = []
            const errors = []
            for (const result of results) {
                if (result.error !== "") {
                    failedPaths.push(result.source)
                    errors.push(result.source + ": " + result.error)
                } else outputs.push(result.output)
            }
            root.failedPaths = failedPaths
            root.errorText = errors.join("\n")
            root.finished(succeeded, failed, outputs)
            if (failed === 0) root.close()
        }
        function onFailed(message, jobId) {
            if (jobId !== root.jobGeneration || !root.visible) return
            root.running = false
            root.errorText = message
        }
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
                if (mouse.button === Qt.LeftButton && !root.running) {
                    root.close()
                }
            }
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(620, root.width - 60)
        height: Math.min(root.height - 40, column.implicitHeight + 40)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.97)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Column {
            id: column
            x: 20
            y: 20
            width: parent.width - 40
            spacing: 12

            Text {
                text: "Correct " + root.fileCount + (root.fileCount === 1 ? " file" : " files")
                font.family: Theme.fontFamily
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.brightForeground
            }
            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.running
                      ? "Saving " + root.doneCount + " of " + root.fileCount + "…"
                      : "Rotate, flip or resize each selected picture as its own copy. "
                        + "The originals are never changed."
                        + (root.hasAnimation ? " Animated pictures save only their first frame." : "")
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
            }

            Flow {
                width: parent.width
                spacing: 6

                PillButton {
                    label: "Rotate left"
                    enabled: !root.running
                    onClicked: root.quarterTurns = ((root.quarterTurns - 1) % 4 + 4) % 4
                }
                PillButton {
                    label: "Rotate right"
                    enabled: !root.running
                    onClicked: root.quarterTurns = (root.quarterTurns + 1) % 4
                }
                PillButton {
                    label: "Flip H"
                    active: root.flipHorizontal
                    enabled: !root.running
                    onClicked: root.flipHorizontal = !root.flipHorizontal
                }
                PillButton {
                    label: "Flip V"
                    active: root.flipVertical
                    enabled: !root.running
                    onClicked: root.flipVertical = !root.flipVertical
                }

                Item { width: 12; height: 1 }

                PillButton {
                    label: "Original size"
                    active: root.targetWidth === 0
                    enabled: !root.running
                    onClicked: root.targetWidth = 0
                }
                PillButton {
                    label: "1920 wide"
                    active: root.targetWidth === 1920
                    enabled: !root.running
                    onClicked: root.targetWidth = 1920
                }
                PillButton {
                    label: "1280 wide"
                    active: root.targetWidth === 1280
                    enabled: !root.running
                    onClicked: root.targetWidth = 1280
                }
            }

            ScrollView {
                width: parent.width
                height: Math.min(120, Math.max(0, root.height - 330))
                visible: root.errorText !== ""
                clip: true
                TextArea {
                    objectName: "batchFailures"
                    text: root.errorText
                    readOnly: true
                    wrapMode: TextEdit.Wrap
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    color: Theme.red
                    background: null
                }
            }

            Row {
                anchors.right: parent.right
                spacing: 8

                PillButton {
                    label: "Cancel"
                    onClicked: root.close()
                }
                PillButton {
                    label: "Retry failed"
                    visible: root.failedPaths.length > 0
                    enabled: !root.running && !ImageEdit.busy
                    onClicked: {
                        root.paths = root.failedPaths
                        root.failedPaths = []
                        root.apply()
                    }
                }
                PillButton {
                    objectName: "batchApply"
                    label: root.running ? "Working…" : root.hasAnimation ? "Save first frames" : "Apply"
                    active: !root.running && !ImageEdit.busy && root.fileCount > 0
                    onClicked: root.apply()
                }
            }
        }
    }

    Keys.onEscapePressed: if (!root.running) root.close()
}
