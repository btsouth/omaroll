import QtQuick

// One setting: its name, a sentence about it, and its control at the end of
// the row. When the control is a switch, a click on the words flips it too.
Item {
    id: root

    property string title: ""
    property string detail: ""
    property color detailColor: Theme.mutedText
    // Names the sentence for tests and tools that read it back.
    property string detailObjectName: ""
    // A switch the words also toggle.
    property SettingSwitch toggle: null
    default property alias controls: trailing.data

    // Controls wider than about half the row go under the words instead.
    readonly property bool stacked: trailing.implicitWidth > root.width * 0.5

    width: parent ? parent.width : implicitWidth
    implicitHeight: stacked ? words.implicitHeight + 8 + trailing.implicitHeight
                            : Math.max(words.implicitHeight, trailing.implicitHeight)

    Column {
        id: words
        x: 0
        y: root.stacked ? 0 : Math.round((root.height - height) / 2)
        width: root.stacked || trailing.implicitWidth === 0
               ? root.width : root.width - trailing.implicitWidth - 16
        spacing: 3

        Text {
            width: parent.width
            visible: root.title !== ""
            text: root.title
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 13
            color: Theme.foreground
        }
        Text {
            objectName: root.detailObjectName
            width: parent.width
            visible: root.detail !== ""
            text: root.detail
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
            lineHeight: 1.15
            color: root.detailColor
        }

        TapHandler {
            enabled: root.toggle !== null && root.toggle.enabled
            onTapped: root.toggle.toggled()
        }
        HoverHandler {
            enabled: root.toggle !== null
            cursorShape: Qt.PointingHandCursor
        }
    }

    Row {
        id: trailing
        x: root.stacked ? 0 : root.width - implicitWidth
        y: root.stacked ? words.implicitHeight + 8 : Math.round((root.height - implicitHeight) / 2)
        spacing: 6
    }
}
