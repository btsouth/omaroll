import QtQuick
import QtQuick.Controls.Basic

// Values for the search token being typed: tag:, album:, in:, camera:,
// lens:, type:, before: and after:. The field keeps the keyboard; it moves
// the highlight and accepts through the functions here.
Popup {
    id: root

    // Rows from Captures.searchSuggestions().
    property var rows: []
    property int highlighted: -1
    readonly property bool hasRows: rows.length > 0

    signal accepted(var row)

    function shade(base, amount) {
        return Qt.rgba(base.r, base.g, base.b, amount)
    }

    function move(step) {
        if (rows.length === 0) return
        highlighted = highlighted < 0 ? (step > 0 ? 0 : rows.length - 1)
                                      : (highlighted + step + rows.length) % rows.length
        list.positionViewAtIndex(highlighted, ListView.Contain)
    }

    // The highlighted row, or the first when nothing is highlighted and
    // |fallback| asks for one (Tab completes the first suggestion).
    function accept(fallback) {
        const index = highlighted >= 0 ? highlighted : (fallback ? 0 : -1)
        if (index < 0 || index >= rows.length) return false
        root.accepted(rows[index])
        return true
    }

    onRowsChanged: highlighted = -1
    padding: 4
    // The search field keeps focus and handles the keys.
    focus: false
    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    implicitHeight: Math.min(list.contentHeight, 8 * 30) + topPadding + bottomPadding

    background: Rectangle {
        color: root.shade(Theme.background, 0.98)
        border.width: 1
        border.color: root.shade(Theme.foreground, 0.16)
        radius: Theme.cornerRadius > 0 ? Theme.cornerRadius : 3
    }

    contentItem: ListView {
        id: list
        objectName: "searchSuggestionList"
        implicitHeight: contentHeight
        clip: true
        model: root.rows
        currentIndex: root.highlighted
        boundsBehavior: Flickable.StopAtBounds
        Accessible.role: Accessible.List
        Accessible.name: "Search suggestions"

        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            readonly property bool current: index === root.highlighted
            width: ListView.view.width
            height: 30
            radius: Theme.cornerRadius > 0 ? Math.min(Theme.cornerRadius, 3) : 2
            color: current ? root.shade(Theme.accent, 0.20)
                           : rowHover.hovered ? root.shade(Theme.foreground, 0.07) : "transparent"
            Accessible.role: Accessible.ListItem
            Accessible.name: modelData.label
            Accessible.selected: current

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: detailText.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.label
                elide: Text.ElideMiddle
                font.family: Theme.fontFamily
                font.pixelSize: 12
                font.weight: row.current ? Font.DemiBold : Font.Normal
                color: row.current ? Theme.brightForeground : Theme.foreground
            }
            Text {
                id: detailText
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.detail
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: Theme.mutedText
            }
            HoverHandler { id: rowHover }
            // A MouseArea accepts the press, so it never reaches a tile
            // under the list. It takes no focus, so the search field keeps
            // the keyboard.
            MouseArea {
                objectName: "searchSuggestionRow"
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                onClicked: function (mouse) {
                    if (mouse.button !== Qt.LeftButton) return
                    root.highlighted = row.index
                    root.accept(false)
                }
            }
        }
    }
}
