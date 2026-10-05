import QtQuick
import QtQuick.Controls.Basic

Popup {
    id: root
    property bool video: false
    property bool animated: false
    // One model for the grouped list. Rows only describe keys implemented here.
    readonly property var groups: [
        {title: "Navigation", rows: [
            {keys: "← / →", label: "Previous / next file", still: true},
            {keys: "Page Up / Page Down", label: "Previous / next file"},
            {keys: "Home / End", label: root.video ? "Start / end of video" : "First / last file"},
            {keys: "Enter", label: "Open in library"},
            {keys: "B", label: "Show / hide filmstrip"},
            {keys: "F5", label: "Start / stop slideshow"},
            {keys: "Esc", label: "Close help, then stop slideshow, leave full screen, close details or viewer"}]},
        {title: "Zoom", still: true, rows: [
            {keys: "+ / −", label: "Zoom in / out"},
            {keys: "0", label: "Fit"},
            {keys: "1 / double click", label: "Toggle actual size / fit"},
            {keys: "W", label: "Fit width, starting at the top"},
            {keys: "Shift + arrows", label: "Pan a zoomed picture"},
            {keys: "R / Shift+R", label: "Rotate right / left"}]},
        {title: "Video", video: true, rows: [
            {keys: "Space / K", label: "Play / pause"},
            {keys: "← / →", label: "Seek five seconds"},
            {keys: "J / L", label: "Seek ten seconds"},
            {keys: "↑ / ↓", label: "Volume up / down"},
            {keys: "M", label: "Mute / unmute"},
            {keys: "[ / ]", label: "Decrease / increase speed"},
            {keys: "Backspace", label: "Normal speed"},
            {keys: "C", label: "Cycle subtitles"},
            {keys: "T / G / P", label: "Trim / save frame / open in mpv"}]},
        {title: "Actions", rows: [
            {keys: "Space / K", label: "Play / pause animation", animation: true},
            {keys: "F / F11", label: "Toggle full screen"},
            {keys: "I", label: "Show / hide details"},
            {keys: "Y / Ctrl+C", label: root.video ? "Copy file" : "Copy image"},
            {keys: "Ctrl+Shift+C", label: "Copy path"},
            {keys: "Ctrl+O", label: "Choose another application"},
            {keys: "Menu", label: "Show the action menu"},
            {keys: "S", label: "Send"},
            {keys: "A", label: "Annotate", still: true},
            {keys: "D / Shift+D", label: "Open RAW editor / choose editor", still: true},
            {keys: "V", label: "Toggle favourite"},
            {keys: "Alt+1 to Alt+5 / Alt+0", label: "Rate / clear rating"},
            {keys: "Ctrl+Z", label: "Undo"},
            {keys: "Del / Shift+Del", label: "Trash / delete permanently"},
            {keys: "Ctrl+W / Ctrl+Q", label: "Close viewer / quit Omaroll"},
            {keys: "? / F1", label: "Show / hide shortcuts"}]}
    ]
    function applicable(row) {
        return (!row.still || !root.video) && (!row.video || root.video)
            && (!row.animation || root.animated)
    }
    parent: Overlay.overlay
    width: Math.min(560, parent.width - 24)
    height: Math.min(620, parent.height - 24)
    x: (parent.width - width) / 2
    y: (parent.height - height) / 2
    modal: true
    focus: true
    onOpened: contentItem.forceActiveFocus()
    padding: 16
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: Rectangle {
        color: Theme.background
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 4
        border.width: 1
        border.color: Theme.mutedText
    }
    contentItem: Item {
        Keys.onPressed: function(event) {
            if (event.key === Qt.Key_Question || event.key === Qt.Key_F1 || event.key === Qt.Key_Escape) {
                root.close()
                event.accepted = true
                return
            }
            const flick = scroll.contentItem
            const maximum = Math.max(0, flick.contentHeight - flick.height)
            let target = flick.contentY
            switch (event.key) {
            case Qt.Key_Down: target += 80; break
            case Qt.Key_Up: target -= 80; break
            case Qt.Key_PageDown: target += flick.height * 0.8; break
            case Qt.Key_PageUp: target -= flick.height * 0.8; break
            case Qt.Key_Home: target = 0; break
            case Qt.Key_End: target = maximum; break
            default: return
            }
            flick.contentY = Math.max(0, Math.min(maximum, target))
            event.accepted = true
        }
        Text {
            id: heading
            text: "Viewer shortcuts"
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
        ScrollView {
            id: scroll
            objectName: "viewerShortcutScroll"
            anchors.top: heading.bottom
            anchors.topMargin: 16
            anchors.bottom: parent.bottom
            width: parent.width
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            Column {
                width: scroll.availableWidth
                spacing: 16
                Repeater {
                    model: root.groups.filter(root.applicable)
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
                            model: parent.modelData.rows.filter(root.applicable)
                            Column {
                                required property var modelData
                                width: parent.width
                                spacing: 2
                                Text {
                                    width: parent.width
                                    text: parent.modelData.keys
                                    wrapMode: Text.WordWrap
                                    color: Theme.brightForeground
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
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
