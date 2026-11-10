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

    Text {
        id: link
        objectName: "textSearchSettingsLink"
        anchors.left: label.right
        anchors.leftMargin: 12
        anchors.verticalCenter: parent.verticalCenter
        text: "Text settings"
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.underline: linkHover.hovered
        color: Theme.accent
        Accessible.role: Accessible.Button
        Accessible.name: "Text search settings"
        Accessible.onPressAction: root.settingsRequested()
        HoverHandler { id: linkHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.settingsRequested() }
    }
}
