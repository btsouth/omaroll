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
    // The key that does the same, drawn as a key the way the shortcut list
    // draws it, so "Videos" and 5 do not read as five videos.
    property string shortcut: ""
    // Long tips wrap rather than run across the window, and never wider
    // than the window itself allows.
    readonly property real windowWidth: owner && owner.Window.window ? owner.Window.width : 0
    readonly property real maximumWidth: windowWidth > 0
                                         ? Math.max(120, Math.min(420, windowWidth - 24)) : 420
    contentItem: Item {
        readonly property real keyRoom: keyCap.visible ? keyCap.width + 10 : 0
        implicitWidth: Math.min(tipText.implicitWidth,
                                tip.maximumWidth - tip.leftPadding - tip.rightPadding - keyRoom) + keyRoom
        implicitHeight: Math.max(tipText.implicitHeight, keyCap.visible ? keyCap.height : 0)
        Text {
            id: tipText
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - (keyCap.visible ? keyCap.width + 10 : 0)
            text: tip.text
            font: tip.font
            color: Theme.foreground
            wrapMode: Text.WordWrap
        }
        Rectangle {
            id: keyCap
            objectName: "toolTipKey"
            visible: tip.shortcut !== ""
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(20, keyText.implicitWidth + 10)
            height: keyText.implicitHeight + 4
            radius: Math.min(Theme.cornerRadius, 4)
            color: Theme.background
            border.width: 1
            border.color: Qt.rgba(Theme.foreground.r, Theme.foreground.g, Theme.foreground.b, 0.28)
            Text {
                id: keyText
                anchors.centerIn: parent
                text: tip.shortcut
                color: Theme.brightForeground
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }
        }
    }
    onAboutToShow: {
        if (ShortcutCatalog.activeTip && ShortcutCatalog.activeTip !== tip)
            ShortcutCatalog.activeTip.close();
        ShortcutCatalog.activeTip = tip;
    }
    onClosed: if (ShortcutCatalog.activeTip === tip)
        ShortcutCatalog.activeTip = null
}
