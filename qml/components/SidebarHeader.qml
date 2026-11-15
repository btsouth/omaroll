import QtQuick

// A sidebar section's title as a framed button, drawn like the header's
// Folders toggle: a click, Enter or Space collapses or expands the section.
// Read as a heading that says whether it is expanded.
Rectangle {
    id: root

    property string label: ""
    property int count: -1
    property bool expanded: true
    // A title that does not fold, such as Pinned: framed the same, with no
    // chevron and nothing to click.
    property bool collapsible: true
    // Drawn with the keyboard ring; a list row passes its own current state.
    property bool keyboardCurrent: activeFocus
    signal toggled()

    implicitHeight: 28
    radius: Theme.cornerRadius > 0 ? Math.min(6, Theme.cornerRadius) : 3
    readonly property bool hovered: hover.hovered && collapsible
    color: shade(Theme.foreground, hovered ? 0.09 : 0.0)
    border.width: keyboardCurrent ? 2 : 1
    border.color: keyboardCurrent ? Theme.accent
                                  : shade(Theme.foreground, hovered ? 0.20 : 0.12)

    function shade(base, amount) { return Qt.rgba(base.r, base.g, base.b, amount) }

    Accessible.role: Accessible.Heading
    Accessible.name: root.label + (root.count >= 0 ? ", " + root.count : "")
    Accessible.expandable: root.collapsible
    Accessible.expanded: root.collapsible && root.expanded
    Accessible.description: !root.collapsible ? ""
                            : root.expanded ? "Expanded. Enter collapses it" : "Collapsed. Enter expands it"
    Accessible.onPressAction: root.toggled()

    Text {
        id: chevron
        x: 8
        anchors.verticalCenter: parent.verticalCenter
        width: root.collapsible ? 14 : 0
        text: !root.collapsible ? "" : root.expanded ? "▾" : "▸"
        color: Theme.mutedText
        font.family: Theme.fontFamily
        font.pixelSize: 13
        Accessible.ignored: true
    }
    Text {
        anchors.left: chevron.right
        anchors.leftMargin: 2
        anchors.right: countText.left
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: root.label
        elide: Text.ElideRight
        color: Theme.foreground
        font.family: Theme.fontFamily
        font.pixelSize: 11
        font.weight: Font.DemiBold
        Accessible.ignored: true
    }
    Text {
        id: countText
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: root.count >= 0 ? Number(root.count).toLocaleString(Qt.locale(), "f", 0) : ""
        color: Theme.mutedText
        font.family: Theme.fontFamily
        font.pixelSize: 11
        Accessible.ignored: true
    }
    HoverHandler { id: hover; cursorShape: root.collapsible ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: root.collapsible; onTapped: root.toggled() }
}
