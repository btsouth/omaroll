import QtQuick
Item {
    id: root
    property var hints: ["Enter details", "Space act", "M background", "T trim", "V favourite", "Del trash", "/ search", "F9 folders"]
    Row {
        spacing: 16
        Repeater {
            model: root.hints
            Text {
                required property string modelData
                required property int index
                font.family: Theme.fontFamily
                font.pixelSize: 11
                color: Theme.mutedText
                text: modelData
                visible: {
                    let used = implicitWidth
                    for (let i = 0; i < index; ++i) used += measure(root.hints[i]) + 16
                    return used <= root.width
                }
            }
        }
    }
    TextMetrics { id: metrics; font.family: Theme.fontFamily; font.pixelSize: 11 }
    function measure(text) { metrics.text = text; return metrics.advanceWidth }
}
