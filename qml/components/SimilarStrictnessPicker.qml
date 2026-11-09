import QtQuick

// How closely pictures must match to be suggested as similar. Normal suits
// most libraries; Strict keeps only near-identical copies; Loose also finds
// recrops and heavier edits. Screenshots of text are always held to a much
// closer match than photographs.
Row {
    id: root
    spacing: 6

    readonly property var choices: [
        {label: "Strict", tip: "Only near-identical copies"},
        {label: "Normal", tip: "Resized, recompressed or brightened copies and close bursts"},
        {label: "Loose", tip: "Also recrops and heavier edits"}
    ]

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: "Match"
        font.family: Theme.fontFamily
        font.pixelSize: 11
        color: Theme.mutedText
    }

    Repeater {
        model: root.choices

        PillButton {
            required property int index
            required property var modelData
            objectName: "similarStrictness" + modelData.label
            label: modelData.label
            accessibleName: "Similar match " + modelData.label
            toolTip: modelData.tip
            checkable: true
            active: Settings.similarStrictness === index
            onClicked: Settings.similarStrictness = index
        }
    }
}
