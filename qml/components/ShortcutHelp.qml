import QtQuick
import QtQuick.Controls.Basic

Popup {
    id: root
    property bool video: false
    property bool animated: false
    property bool audioChoices: false
    property bool sidecarSubtitles: false
    property string context: "quick"
    property string filter: ""
    readonly property var groups: ShortcutCatalog.groups(context, video, animated, audioChoices, sidecarSubtitles, filter)
    function columnsForWidth(width) {
        const count = width >= 1000 ? 3 : width >= 680 ? 2 : 1;
        let columns = [], sizes = [];
        for (let i = 0; i < count; ++i) {
            columns.push([]);
            sizes.push(0);
        }
        const ordered = groups.slice().sort(function (a, b) {
            return b.rows.length - a.rows.length;
        });
        for (const group of ordered) {
            const index = sizes.indexOf(Math.min.apply(null, sizes));
            columns[index].push(group);
            sizes[index] += group.rows.length + 2;
        }
        return columns;
    }
    parent: Overlay.overlay
    width: Math.min(1320, parent.width - 24)
    height: Math.min(1020, parent.height - 24)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    modal: true
    focus: true
    onOpened: {
        search.text = "";
        root.filter = "";
        contentItem.forceActiveFocus();
    }
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: Theme.background
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        border.width: 1
        border.color: Theme.mutedText
    }
    contentItem: Item {
        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Question || event.key === Qt.Key_F1 || event.key === Qt.Key_Escape) {
                root.close();
                event.accepted = true;
                return;
            }
            const flick = scroll.contentItem;
            const maximum = Math.max(0, flick.contentHeight - flick.height);
            let target = flick.contentY;
            switch (event.key) {
            case Qt.Key_Down:
                target += 80;
                break;
            case Qt.Key_Up:
                target -= 80;
                break;
            case Qt.Key_PageDown:
                target += flick.height * 0.8;
                break;
            case Qt.Key_PageUp:
                target -= flick.height * 0.8;
                break;
            case Qt.Key_Home:
                target = 0;
                break;
            case Qt.Key_End:
                target = maximum;
                break;
            default:
                return;
            }
            flick.contentY = Math.max(0, Math.min(maximum, target));
            event.accepted = true;
        }
        Text {
            id: heading
            text: root.context === "quick" ? "Viewer shortcuts" : root.context === "preview" ? "Preview shortcuts" : "Library shortcuts"
            color: Theme.brightForeground
            font.family: Theme.fontFamily
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }
        PillButton {
            anchors.right: parent.right
            label: "Close"
            onClicked: root.close()
        }
        TextField {
            id: search
            objectName: "shortcutFilter"
            anchors.top: heading.bottom
            anchors.topMargin: 12
            width: parent.width
            placeholderText: "Filter shortcuts"
            color: Theme.foreground
            font.family: Theme.fontFamily
            onTextChanged: root.filter = text
            background: Rectangle {
                color: Theme.surfaceBackground
                radius: Theme.cornerRadius
                border.width: 1
                border.color: Theme.mutedText
            }
        }
        ScrollView {
            id: scroll
            objectName: "viewerShortcutScroll"
            anchors.top: search.bottom
            anchors.topMargin: 12
            anchors.bottom: parent.bottom
            width: parent.width
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Row {
                width: scroll.availableWidth
                spacing: 20
                readonly property int columns: width >= 1000 ? 3 : width >= 680 ? 2 : 1
                Repeater {
                    model: root.columnsForWidth(scroll.availableWidth)
                    Column {
                        required property var modelData
                        width: (scroll.availableWidth - 20 * (parent.columns - 1)) / parent.columns
                        spacing: 16
                        Repeater {
                            model: parent.modelData
                            Column {
                                required property var modelData
                                width: parent.width
                                spacing: 8
                                Text {
                                    text: parent.modelData.title
                                    color: Theme.accent
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                                Repeater {
                                    model: parent.modelData.rows
                                    Column {
                                        required property var modelData
                                        width: parent.width
                                        spacing: 2
                                        Rectangle {
                                            width: Math.min(parent.width, keyText.implicitWidth + 12)
                                            height: keyText.implicitHeight + 6
                                            radius: Math.min(Theme.cornerRadius, 4)
                                            color: Theme.surfaceBackground
                                            border.width: 1
                                            border.color: Theme.mutedText
                                            Text {
                                                id: keyText
                                                anchors.centerIn: parent
                                                width: parent.width - 12
                                                text: parent.parent.modelData.keys
                                                wrapMode: Text.WordWrap
                                                color: Theme.brightForeground
                                                font.family: Theme.fontFamily
                                                font.pixelSize: 12
                                            }
                                        }
                                        Text {
                                            width: parent.width
                                            text: parent.modelData.label
                                            wrapMode: Text.WordWrap
                                            color: Theme.foreground
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 12
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
