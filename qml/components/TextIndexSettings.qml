import QtQuick

// Opt-in background reading of picture text. Off, text is read only while a
// search is open, one picture at a time. On, the library is read after its
// scan settles, a few pictures at a time at the lowest priority, and only on
// mains power when the computer can tell.
Column {
    id: root
    spacing: 16
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

    SettingRow {
        title: "Index text in the background"
        detail: root.status !== "" ? root.status
                : "Off: text is read while you search. On: read once the library is "
                  + "scanned, at low priority and only on mains power."
        detailObjectName: "textIndexStatus"
        toggle: backgroundToggle

        SettingSwitch {
            id: backgroundToggle
            objectName: "backgroundTextIndexToggle"
            anchors.verticalCenter: parent.verticalCenter
            accessibleName: "Index text in the background"
            checked: Settings.backgroundTextIndex
            onToggled: Settings.backgroundTextIndex = !Settings.backgroundTextIndex
        }
    }

    SettingRow {
        visible: Settings.backgroundTextIndex
        title: "Screenshots only"
        detail: "Photos are still read while you search."
        toggle: screenshotsToggle

        SettingSwitch {
            id: screenshotsToggle
            objectName: "textIndexScreenshotsOnlyToggle"
            anchors.verticalCenter: parent.verticalCenter
            accessibleName: "Index screenshots only"
            checked: Settings.textIndexScreenshotsOnly
            onToggled: Settings.textIndexScreenshotsOnly = !Settings.textIndexScreenshotsOnly
        }
    }
}
