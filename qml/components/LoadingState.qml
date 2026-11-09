import QtQuick

// Says the library is still looking, in place of an empty state that would
// not be true yet. A quick scan answers before this appears at all.
Text {
    id: loading

    property bool waiting: false
    property int delay: 250
    property bool shown: false

    visible: waiting && shown
    horizontalAlignment: Text.AlignHCenter
    wrapMode: Text.WordWrap
    text: "Looking for pictures and videos…"
    font.family: Theme.fontFamily
    font.pixelSize: 13
    color: Theme.mutedText
    Accessible.role: Accessible.StaticText
    Accessible.name: text
    onWaitingChanged: if (!waiting) shown = false

    Timer {
        interval: loading.delay
        running: loading.waiting && !loading.shown
        onTriggered: loading.shown = true
    }
}
