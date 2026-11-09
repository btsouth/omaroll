import QtQuick

// Opt-in background reading of picture text. Off, text is read only while a
// search is open, one picture at a time. On, the library is read after its
// scan settles, a few pictures at a time at the lowest priority, and only on
// mains power when the computer can tell.
Column {
    id: root
    spacing: 8
    visible: TextIndex.available

    readonly property string status: {
        if (!Settings.backgroundTextIndex) return ""
        if (TextIndex.pauseReason !== "") return TextIndex.pauseReason
        if (TextIndex.eligibleCount === 0) return "Waiting for the library"
        return (TextIndex.indexedCount >= TextIndex.eligibleCount ? "Text indexed for all "
                : "Text indexed " + TextIndex.indexedCount + " of ")
               + TextIndex.eligibleCount
               + (Settings.textIndexScreenshotsOnly ? " screenshots" : " pictures")
    }

    Row {
        width: parent.width
        spacing: 12

        Column {
            width: parent.width - backgroundToggle.width - 12
            spacing: 2

            Text {
                width: parent.width
                text: "Index text in the background"
                wrapMode: Text.WordWrap
                font.family: Theme.fontFamily
                font.pixelSize: 12
                color: Theme.foreground
            }
            Text {
                objectName: "textIndexStatus"
                width: parent.width
                wrapMode: Text.WordWrap
                text: root.status !== "" ? root.status
                      : "Off: text is read while you search. On: read once the library is "
                        + "scanned, at low priority and only on mains power."
                font.family: Theme.fontFamily
                font.pixelSize: 10
                color: Theme.mutedText
            }
        }

        PillButton {
            id: backgroundToggle
            objectName: "backgroundTextIndexToggle"
            anchors.verticalCenter: parent.verticalCenter
            accessibleName: "Index text in the background"
            label: Settings.backgroundTextIndex ? "On" : "Off"
            checkable: true
            active: Settings.backgroundTextIndex
            onClicked: Settings.backgroundTextIndex = !Settings.backgroundTextIndex
        }
    }

    Row {
        width: parent.width
        spacing: 12
        visible: Settings.backgroundTextIndex

        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - screenshotsToggle.width - 12
            text: "Screenshots only"
            wrapMode: Text.WordWrap
            font.family: Theme.fontFamily
            font.pixelSize: 12
            color: Theme.foreground
        }
        PillButton {
            id: screenshotsToggle
            objectName: "textIndexScreenshotsOnlyToggle"
            anchors.verticalCenter: parent.verticalCenter
            accessibleName: "Index screenshots only"
            label: Settings.textIndexScreenshotsOnly ? "On" : "Off"
            checkable: true
            active: Settings.textIndexScreenshotsOnly
            onClicked: Settings.textIndexScreenshotsOnly = !Settings.textIndexScreenshotsOnly
        }
    }
}
