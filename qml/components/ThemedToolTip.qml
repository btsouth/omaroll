import QtQuick
import QtQuick.Controls.Basic

ToolTip {
    id: tip
    property bool requested: false
    property bool keyboardRequested: false
    property bool focusSuppressed: false
    onBlockedChanged: if (blocked) focusSuppressed = true
    Connections {
        target: tip.owner
        function onActiveFocusChanged() { if (tip.owner.activeFocus) tip.focusSuppressed = false }
    }
    property Item owner: parent
    readonly property bool blocked: owner && owner.Window.window
        && owner.Window.window.tooltipsBlocked === true
    visible: (requested || (keyboardRequested && !focusSuppressed)) && !blocked && owner && owner.visible && owner.enabled
    delay: 600
    timeout: 5000
    font.family: Theme.fontFamily
    font.pixelSize: 12
    padding: 8
    background: Rectangle {
        color: Theme.surfaceBackground
        radius: Theme.cornerRadius
        border.width: 1
        border.color: Theme.mutedText
    }
    contentItem: Text {
        text: tip.text
        font: tip.font
        color: Theme.foreground
        wrapMode: Text.WordWrap
    }
    onAboutToShow: {
        if (ShortcutCatalog.activeTip && ShortcutCatalog.activeTip !== tip)
            ShortcutCatalog.activeTip.close()
        ShortcutCatalog.activeTip = tip
    }
    onClosed: if (ShortcutCatalog.activeTip === tip) ShortcutCatalog.activeTip = null
}
