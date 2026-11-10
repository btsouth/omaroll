import QtQuick

// Said under search results while some pictures' text has not been read yet,
// so a missing match is not mistaken for no match. "Text settings" opens the
// indexing settings, where background reading is turned on.
Rectangle {
    id: root

    signal settingsRequested()

    readonly property bool wanted: TextIndex.available && Captures.searchText.trim().length >= 2
                                   && TextIndex.unreadCount > 0

    visible: wanted
    implicitWidth: label.implicitWidth + link.implicitWidth + 36
    implicitHeight: label.implicitHeight + 12
    radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0.94)
    border.width: 1
    border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.16)

    Text {
        id: label
        objectName: "textSearchNotice"
        anchors.left: parent.left
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, root.width - link.implicitWidth - 36)
        elide: Text.ElideRight
        text: "Text results may be incomplete: "
              + TextIndex.unreadCount + (TextIndex.unreadCount === 1 ? " picture" : " pictures")
              + " not read yet"
              + (TextIndex.indexing ? "" : TextIndex.pauseReason !== ""
                                       ? " (" + TextIndex.pauseReason.toLowerCase() + ")" : "")
        font.family: Theme.fontFamily
        font.pixelSize: 11
        color: Theme.mutedText
    }

    // A link to look at, a button to the keyboard: Tab reaches it, Enter
    // or Space opens the settings, and it shows a ring while focused.
    Text {
        id: link
        objectName: "textSearchSettingsLink"
        anchors.left: label.right
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: "Text settings"
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.underline: linkHover.hovered || link.activeFocus
        color: Theme.accent
        activeFocusOnTab: root.visible
        Accessible.role: Accessible.Button
        Accessible.name: "Text search settings"
        Accessible.description: "Open the text indexing settings"
        Accessible.focusable: true
        Accessible.onPressAction: root.settingsRequested()
        Keys.onPressed: function (event) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                root.settingsRequested()
                event.accepted = true
            }
        }
        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.settingsRequested() }

        Rectangle {
            objectName: "textSearchSettingsFocusRing"
            anchors.fill: parent
            anchors.margins: -3
            visible: link.activeFocus
            color: "transparent"
            radius: 3
            border.width: 2
            border.color: Theme.accent
        }
    }
}
