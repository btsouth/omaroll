import QtQuick
import QtQuick.Controls.Basic

ToolTip {
    id: tip
    property bool requested: false
    property Item owner: parent
    readonly property bool blocked: {
        const overlay = Overlay.overlay
        if (!overlay) return false
        for (let i = 0; i < overlay.children.length; ++i) {
            const child = overlay.children[i]
            if (!child.visible || child === tip.contentItem || child === tip.background
                    || child.width <= 0 || child.height <= 0) continue
            // Popup surfaces reparent their content into the overlay. A tip
            // belongs above that surface only when its owner is inside it.
            let ancestor = tip.owner
            let inside = false
            while (ancestor) { if (ancestor === child) inside = true; ancestor = ancestor.parent }
            if (!inside && child.z >= 0) return true
        }
        return false
    }
    visible: requested && !blocked && owner && owner.visible && owner.enabled
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
