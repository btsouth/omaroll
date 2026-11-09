import QtQuick
import QtQuick.Controls.Basic

ToolTip {
    id: tip
    property bool requested: false
    property bool keyboardRequested: false
    property bool focusSuppressed: false
    // A sheet or a disabled owner hides the keyboard tip that was up, until
    // the keyboard moves on; tips that weren't requested stay available.
    onBlockedChanged: if (blocked)
        focusSuppressed = focusSuppressed || keyboardRequested
    onKeyboardRequestedChanged: if (!keyboardRequested)
        focusSuppressed = false
    Connections {
        target: tip.owner
        function onEnabledChanged() {
            if (!tip.owner.enabled) tip.focusSuppressed = tip.focusSuppressed || tip.keyboardRequested;
        }
        function onActiveFocusChanged() {
            if (tip.owner.activeFocus)
                tip.focusSuppressed = false;
        }
    }
    property Item owner: parent
    readonly property bool blocked: owner && owner.Window.window && owner.Window.window.tooltipsBlocked === true
    visible: text !== "" && (requested || (keyboardRequested && !focusSuppressed)) && !blocked && owner && owner.visible && owner.enabled
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
            ShortcutCatalog.activeTip.close();
        ShortcutCatalog.activeTip = tip;
    }
    onClosed: if (ShortcutCatalog.activeTip === tip)
        ShortcutCatalog.activeTip = null
}
