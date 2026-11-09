import QtQuick
import QtQuick.Controls.Basic

// A small toggle in the filter bar. Deliberately quiet: the pictures are the
// loud part of this window, and a row of bright chips would compete with them.
Item {
    id: root

    property string label: ""
    property string accessibleName: label
    property bool checkable: false
    property real maximumWidth: Infinity
    property bool active: false
    property bool floating: false
    // Over a picture or video: dark in every theme (see Osd).
    property bool osd: false
    // A line icon from Icon in place of the label, which then names it for
    // assistive technology and the tooltip.
    property string icon: ""
    // One part of a segmented control: the frame around the row draws the
    // outline, and only the chosen part is filled, inset from it.
    property bool segment: false
    readonly property color textColour: root.osd ? Osd.foreground : Theme.foreground
    readonly property color brightColour: root.osd ? Osd.brightForeground : Theme.brightForeground
    readonly property color accentColour: root.osd ? Osd.accent : Theme.accent
    property int badge: -1
    property string toolTip: ""
    property string shortcut: ""
    readonly property string resolvedToolTip: {
        const action = root.toolTip !== "" ? root.toolTip : root.label
        return root.shortcut !== ""
               ? action + "  ·  " + root.shortcut
               : root.toolTip
    }

    signal clicked()

    activeFocusOnTab: true
    opacity: root.enabled ? 1 : 0.42
    Accessible.role: Accessible.Button
    Accessible.name: root.accessibleName
    Accessible.checkable: root.checkable
    Accessible.checked: root.checkable && root.active
    Accessible.description: root.shortcut !== "" ? "Shortcut " + root.shortcut : ""
    Accessible.onPressAction: if (root.enabled) root.clicked()

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    TextMetrics {
        id: labelMetrics
        text: root.label
        font.family: Theme.fontFamily
        font.pixelSize: 12
        font.weight: root.active || root.floating ? Font.DemiBold : Font.Normal
    }

    implicitWidth: root.icon !== "" ? implicitHeight + (root.floating ? 0 : 4)
                   : Math.min(root.maximumWidth, labelMetrics.advanceWidth
                           + (root.badge >= 0 ? badgeText.implicitWidth + 6 : 0)
                           + (floating ? 26 : 22))
    implicitHeight: floating ? 34 : 26

    Behavior on opacity { NumberAnimation { duration: 120 } }

    Rectangle {
        anchors.fill: parent
        anchors.margins: root.segment ? 2 : 0
        radius: Theme.cornerRadius > 0 ? Math.max(2, Theme.cornerRadius - (root.segment ? 2 : 0)) : 3
        color: root.active
               ? root.shade(root.accentColour, root.floating ? 0.42 : 0.18)
               : (root.floating
                  ? (root.osd ? root.shade(Osd.panel, hover.hovered ? 0.96 : 0.84)
                              : root.shade(Theme.background, hover.hovered ? 0.92 : 0.72))
                  : root.shade(root.textColour, hover.hovered ? 0.09 : 0.0))
        border.width: root.activeFocus ? 2 : (root.segment && !root.active ? 0 : 1)
        border.color: root.active
                      ? root.shade(root.accentColour, root.floating ? 0.90 : 0.55)
                      : root.activeFocus
                        ? root.accentColour
                        : root.shade(root.textColour,
                                     root.floating ? (hover.hovered ? 0.64 : 0.46)
                                                   : (hover.hovered ? 0.20 : 0.12))

        Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
        Behavior on border.color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }

        Icon {
            anchors.centerIn: parent
            visible: root.icon !== ""
            name: root.icon
            size: root.floating ? 18 : 15
            color: root.active || hover.hovered ? root.brightColour
                   : (root.floating ? root.textColour : (root.osd ? Osd.mutedText : Theme.mutedText))
        }

        Row {
            id: content
            visible: root.icon === ""
            anchors.centerIn: parent
            width: Math.max(0, root.width - (root.floating ? 26 : 22))
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.label
                width: Math.max(0, content.width - (badgeText.visible ? badgeText.implicitWidth + content.spacing : 0))
                elide: Text.ElideRight
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: root.active || root.floating ? Font.DemiBold : Font.Normal
                color: root.active ? root.brightColour
                                   : (root.floating ? root.textColour
                                                    : (root.osd ? Osd.mutedText : Theme.mutedText))

                Behavior on color { ColorAnimation { duration: 140; easing.type: Easing.OutQuad } }
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                id: badgeText
                visible: root.badge >= 0
                text: root.badge
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: root.shade(root.active ? root.brightColour : root.textColour, 0.5)
            }
        }

        HoverHandler {
            id: hover
            cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        }

        ThemedToolTip {
            owner: root
            requested: hover.hovered
            keyboardRequested: root.activeFocus && root.Window.window && root.Window.window.keyboardNavigation === true
            text: root.resolvedToolTip
            delay: 500
        }

        TapHandler {
            enabled: root.enabled
            // Buttons act on every release. singleTapped deliberately waits
            // for and excludes a possible double-tap, which drops clicks when
            // someone moves quickly through Previous or Next.
            onTapped: root.clicked()
        }
    }

    Keys.onPressed: function(event) {
        if (root.enabled && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                             || event.key === Qt.Key_Space)) {
            root.clicked()
            event.accepted = true
        }
    }
}
