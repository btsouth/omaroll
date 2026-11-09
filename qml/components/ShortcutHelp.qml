import QtQuick
import QtQuick.Controls.Basic

// Every shortcut for the viewer, the library or the preview, from the
// shared table. One row per shortcut: what it does on the left, its keys on
// the right, one cap per key. The sheet is as large as what it lists, up to
// the window, and scrolls only when it truly cannot fit.
Popup {
    id: root
    property bool video: false
    property bool animated: false
    property bool audioChoices: false
    property bool sidecarSubtitles: false
    property string context: "quick"
    property string filter: ""
    readonly property var groups: ShortcutCatalog.groups(context, video, animated, audioChoices, sidecarSubtitles, filter)

    readonly property int columnGap: 28
    readonly property int preferredColumnWidth: 400
    readonly property int minimumColumnWidth: 340
    readonly property real room: Math.max(0, (parent ? parent.width : 0) - 24)
    readonly property int columnCount: {
        const fit = Math.floor((room - 2 * padding + columnGap) / (minimumColumnWidth + columnGap))
        return Math.max(1, Math.min(3, fit, groups.length))
    }

    // Groups into columns, longest first, each into the shortest column so far.
    function columnsFor(count) {
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
    width: Math.min(room, columnCount * preferredColumnWidth + columnGap * (columnCount - 1) + 2 * padding)
    readonly property real naturalHeight: heading.implicitHeight + 12 + search.implicitHeight + 12
                                          + Math.max(columnsRow.implicitHeight, 40) + 2 * padding
    height: Math.min(naturalHeight, (parent ? parent.height : 0) - 24)
    x: Math.round(((parent ? parent.width : 0) - width) / 2)
    y: Math.round(((parent ? parent.height : 0) - height) / 2)
    modal: true
    focus: true
    onOpened: {
        search.text = "";
        root.filter = "";
        scroll.contentItem.contentY = 0;
        contentItem.forceActiveFocus();
    }
    padding: 18
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: Theme.background
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        border.width: 1
        border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.22)
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
                // Typing starts filtering, so "trash" right after F1 finds it.
                if (event.text.length > 0 && event.text.trim() !== ""
                        && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                    search.forceActiveFocus();
                    search.insert(search.cursorPosition, event.text);
                    event.accepted = true;
                }
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
            anchors.verticalCenter: heading.verticalCenter
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
            placeholderTextColor: Theme.mutedText
            Accessible.name: "Filter shortcuts"
            color: Theme.foreground
            font.family: Theme.fontFamily
            onTextChanged: root.filter = text
            background: Rectangle {
                color: Theme.surfaceBackground
                radius: Theme.cornerRadius
                border.width: 1
                border.color: search.activeFocus ? Theme.accent
                                                 : Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.22)
            }
        }
        Text {
            anchors.top: search.bottom
            anchors.topMargin: 14
            visible: root.groups.length === 0
            text: "No shortcut matches “" + root.filter + "”"
            color: Theme.mutedText
            font.family: Theme.fontFamily
            font.pixelSize: 12
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
                id: columnsRow
                width: scroll.availableWidth
                spacing: root.columnGap
                Repeater {
                    model: root.columnsFor(root.columnCount)
                    Column {
                        required property var modelData
                        width: Math.floor((columnsRow.width - root.columnGap * (root.columnCount - 1))
                                          / root.columnCount) - 1
                        spacing: 16
                        Repeater {
                            model: parent.modelData
                            Column {
                                required property var modelData
                                width: parent.width
                                spacing: 2
                                Text {
                                    text: parent.modelData.title
                                    color: Theme.accent
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    bottomPadding: 4
                                }
                                Repeater {
                                    model: parent.modelData.rows
                                    // One shortcut. Its keys as one string, for
                                    // assistive technology and for the tests.
                                    Item {
                                        id: shortcutRow
                                        required property var modelData
                                        readonly property string text: modelData.keys
                                        // Keys that would crowd the label go under it.
                                        readonly property bool stacked: caps.implicitWidth > width * 0.6
                                        width: parent.width
                                        height: stacked ? label.implicitHeight + 4 + caps.implicitHeight + 4
                                                        : Math.max(label.implicitHeight, caps.implicitHeight) + 6
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: modelData.label + ", " + modelData.keys

                                        Text {
                                            id: label
                                            anchors.left: parent.left
                                            y: shortcutRow.stacked ? 2 : Math.round((shortcutRow.height - implicitHeight) / 2)
                                            width: shortcutRow.stacked ? parent.width
                                                                       : parent.width - caps.implicitWidth - 14
                                            text: shortcutRow.modelData.label
                                            wrapMode: Text.WordWrap
                                            color: Theme.foreground
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 12
                                        }
                                        Row {
                                            id: caps
                                            anchors.right: parent.right
                                            y: shortcutRow.stacked ? label.implicitHeight + 6
                                                                   : Math.round((shortcutRow.height - implicitHeight) / 2)
                                            spacing: 6
                                            Repeater {
                                                model: shortcutRow.modelData.caps
                                                Row {
                                                    id: alternative
                                                    required property var modelData
                                                    required property int index
                                                    spacing: 3
                                                    Text {
                                                        visible: alternative.index > 0
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        text: "/"
                                                        color: Theme.mutedText
                                                        font.family: Theme.fontFamily
                                                        font.pixelSize: 11
                                                        rightPadding: 3
                                                    }
                                                    Repeater {
                                                        model: alternative.modelData
                                                        Rectangle {
                                                            required property string modelData
                                                            width: Math.max(22, capText.implicitWidth + 12)
                                                            height: capText.implicitHeight + 6
                                                            radius: Math.min(Theme.cornerRadius, 4)
                                                            color: Theme.surfaceBackground
                                                            border.width: 1
                                                            border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g,
                                                                                  Theme.foreground.b, 0.28)
                                                            Text {
                                                                id: capText
                                                                anchors.centerIn: parent
                                                                text: parent.modelData
                                                                color: Theme.brightForeground
                                                                font.family: Theme.fontFamily
                                                                font.pixelSize: 11
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
                }
            }
        }
        // More below: the last rows fade rather than stop at a hard edge.
        Rectangle {
            objectName: "shortcutHelpFade"
            anchors.left: scroll.left
            anchors.right: scroll.right
            anchors.bottom: scroll.bottom
            height: 32
            readonly property var flick: scroll.contentItem
            visible: flick.contentHeight > flick.height + 1
                     && flick.contentY < flick.contentHeight - flick.height - 1
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0) }
                GradientStop { position: 1.0; color: Theme.background }
            }
        }
    }
}
