import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root

    property string path: ""
    property string fileName: ""
    property string copyMessage: ""
    readonly property bool loading: TextIndex.reviewPath === root.path && TextIndex.reviewing
    readonly property string errorMessage: TextIndex.reviewPath === root.path
                                                   ? TextIndex.reviewError : ""

    function syncText() {
        if (TextIndex.reviewPath === root.path && !TextIndex.reviewing) {
            textArea.text = TextIndex.reviewText
            textArea.cursorPosition = 0
            if (textArea.text !== "" && root.visible) {
                Qt.callLater(textArea.forceActiveFocus)
            }
        }
    }

    function open(filePath, name) {
        copyMessage = ""
        path = filePath
        fileName = name
        textArea.text = ""
        visible = true
        forceActiveFocus()
        TextIndex.recognize(path)
        syncText()
    }

    function retry() {
        copyMessage = ""
        textArea.text = ""
        TextIndex.recognize(path, true)
    }

    function close() {
        copyMessage = ""
        TextIndex.cancelReview()
        visible = false
    }

    function copySelection() {
        if (textArea.selectedText !== "") {
            textArea.copy()
            confirmCopy("Copied selection")
        }
    }

    function copyAll() {
        if (textArea.text === "") {
            return
        }
        textArea.select(0, textArea.length)
        textArea.copy()
        textArea.select(0, 0)
        textArea.forceActiveFocus()
        confirmCopy("Copied all text")
    }

    function confirmCopy(message) {
        copyMessage = message
        copyStatus.Accessible.announce(message, Accessible.Polite)
        copyMessageTimer.restart()
    }

    Timer {
        id: copyMessageTimer
        interval: 3000
        onTriggered: root.copyMessage = ""
    }

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    visible: false
    anchors.fill: parent
    focus: visible

    Connections {
        target: TextIndex
        function onReviewChanged() { root.syncText() }
    }

    WheelHandler {
        target: null
        enabled: root.visible
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function (event) { event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.62)

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
        id: panel
        anchors.centerIn: parent
        width: Math.min(700, root.width - 60)
        height: Math.min(540, root.height - 60)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.20)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            preventStealing: true
        }

        Text {
            id: title
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 22
            text: "Extract text"
            font.family: Theme.fontFamily
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.brightForeground
        }

        Text {
            id: subtitle
            anchors.left: title.left
            anchors.right: title.right
            anchors.top: title.bottom
            anchors.topMargin: 5
            text: root.fileName + "  ·  edits here do not change the image"
            elide: Text.ElideMiddle
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.mutedText
        }

        Rectangle {
            id: textFrame
            anchors.left: title.left
            anchors.right: title.right
            anchors.top: subtitle.bottom
            anchors.bottom: copyStatus.top
            anchors.topMargin: 16
            anchors.bottomMargin: 14
            radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
            color: root.shade(Theme.foreground, 0.05)
            border.width: 1
            border.color: textArea.activeFocus
                          ? root.shade(Theme.accent, 0.60)
                          : root.shade(Theme.foreground, 0.14)

            ScrollView {
                anchors.fill: parent
                anchors.margins: 8
                visible: !root.loading && root.errorMessage === "" && textArea.text !== ""
                clip: true

                TextArea {
                    id: textArea
                    objectName: "ocrTextArea"
                    width: parent.width
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    padding: 6
                    font.family: Theme.fontFamily
                    font.pixelSize: 13
                    color: Theme.foreground
                    selectionColor: root.shade(Theme.accent, 0.52)
                    selectedTextColor: Theme.brightForeground
                    background: null
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 48
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: root.loading || root.errorMessage !== "" || textArea.text === ""
                text: root.loading ? "Reading text…"
                      : root.errorMessage !== "" ? root.errorMessage
                      : "No text found"
                font.family: Theme.fontFamily
                font.pixelSize: 13
                color: root.errorMessage !== "" ? Theme.red : Theme.mutedText
            }
        }

        Text {
            id: copyStatus
            objectName: "ocrCopyStatus"
            anchors.left: title.left
            anchors.right: title.right
            anchors.bottom: controls.top
            anchors.bottomMargin: 10
            height: 16
            text: root.copyMessage
            font.family: Theme.fontFamily
            font.pixelSize: 11
            color: Theme.accent
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Row {
            id: controls
            anchors.right: title.right
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 18
            spacing: 8

            PillButton {
                label: "Retry"
                visible: !root.loading
                onClicked: root.retry()
            }
            PillButton {
                objectName: "copyOcrSelection"
                label: "Copy selection"
                enabled: textArea.selectedText !== ""
                onClicked: root.copySelection()
            }
            PillButton {
                objectName: "copyAllOcrText"
                label: "Copy all"
                active: enabled
                enabled: textArea.text !== ""
                onClicked: root.copyAll()
            }
            PillButton {
                label: "Close"
                onClicked: root.close()
            }
        }
    }

    Keys.onPressed: function (event) {
        if (event.key === Qt.Key_Escape) {
            root.close()
            event.accepted = true
        }
    }
}
