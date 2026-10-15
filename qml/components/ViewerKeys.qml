import QtQuick

// The quick viewer's keyboard. Each key asks the viewer for one of its own
// actions; video, pictures and animations share keys where that reads well.
Item {
    // The Viewer window whose actions the keys perform.
    required property var viewer

    Keys.onPressed: function (event) {
        if (viewer.choosingEditor) {
            event.accepted = false
            return
        }
        const control = (event.modifiers & Qt.ControlModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const video = Session.isVideo
        let handled = true

        if (video && shift && !control && !alt && event.key === Qt.Key_L) {
            if (!event.isAutoRepeat) viewer.toggleVideoLoop()
        } else if (video && shift && !control && !alt && event.key === Qt.Key_A) {
            viewer.cycleAudioTrack()
        } else if (alt && event.key >= Qt.Key_0 && event.key <= Qt.Key_5) {
            viewer.rate(event.key - Qt.Key_0)
        } else if (control && event.key === Qt.Key_Z) {
            if (Settings.canUndo()) {
                Settings.undo()
                viewer.say("Undone")
            }
        } else if (control && event.key === Qt.Key_C) {
            if (shift) viewer.copyPath(false)
            else viewer.perform("copy")
        } else if (control && event.key === Qt.Key_O) {
            viewer.perform("open-with")
        } else if (control && event.key === Qt.Key_W) {
            viewer.close()
        } else if (control && event.key === Qt.Key_Q) {
            Qt.quit()
        } else if (shift && !control && !alt && !video
                   && [Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down].indexOf(event.key) >= 0) {
            viewer.panImage(event.key === Qt.Key_Left ? -80 : event.key === Qt.Key_Right ? 80 : 0,
                          event.key === Qt.Key_Up ? -80 : event.key === Qt.Key_Down ? 80 : 0)
        } else if (control || alt) {
            handled = false
        } else {
            switch (event.key) {
            // Tab would walk focus into the details card and leave the
            // viewer's keys behind.
            case Qt.Key_Tab:
            case Qt.Key_Backtab:
                break
            case Qt.Key_Escape:
                if (viewer.slideshowRunning) viewer.setSlideshow(false)
                else if (viewer.fullScreen) viewer.setFullScreen(false)
                else if (viewer.infoOpen) viewer.infoOpen = false
                else viewer.close()
                break
            case Qt.Key_Left:
            case Qt.Key_Right: {
                const direction = event.key === Qt.Key_Left ? -1 : 1
                if (video) viewer.seek(direction * 5000)
                else viewer.step(direction)
                break
            }
            case Qt.Key_PageUp:
            case Qt.Key_PageDown:
                viewer.step(event.key === Qt.Key_PageUp ? -1 : 1)
                break
            case Qt.Key_Home:
            case Qt.Key_End:
                if (video && viewer.player) {
                    viewer.seekTo(event.key === Qt.Key_Home ? 0 : viewer.player.duration)
                } else {
                    Session.jump(event.key === Qt.Key_Home ? 0 : Session.count - 1)
                }
                break
            case Qt.Key_Space:
            case Qt.Key_K:
                if (video || Session.isAnimated) viewer.togglePlayback()
                else handled = false
                break
            case Qt.Key_J:
            case Qt.Key_L:
                if (video) viewer.seek(event.key === Qt.Key_J ? -10000 : 10000)
                else handled = false
                break
            case Qt.Key_Comma:
            case Qt.Key_Period:
                if (video) viewer.stepFrame(event.key === Qt.Key_Comma ? -1 : 1)
                else handled = false
                break
            case Qt.Key_Up:
            case Qt.Key_Down:
                if (video) viewer.adjustVolume(event.key === Qt.Key_Up ? 0.05 : -0.05)
                else handled = false
                break
            case Qt.Key_M:
                if (video) {
                    Settings.videoMuted = !Settings.videoMuted
                    viewer.say(Settings.videoMuted ? "Muted" : "Sound on")
                } else {
                    handled = false
                }
                break
            case Qt.Key_C:
                if (video) viewer.cycleSubtitles()
                else handled = false
                break
            case Qt.Key_Z:
            case Qt.Key_X:
                if (video) viewer.adjustSubtitleOffset(event.key === Qt.Key_Z ? -500 : 500)
                else handled = false
                break
            case Qt.Key_BracketLeft:
            case Qt.Key_BracketRight:
                if (video && viewer.player) {
                    viewer.setPlaybackRate(viewer.player.playbackRate
                        + (event.key === Qt.Key_BracketLeft ? -0.25 : 0.25))
                } else {
                    handled = false
                }
                break
            case Qt.Key_Backspace:
                if (video && viewer.player) {
                    viewer.setPlaybackRate(1)
                } else {
                    handled = false
                }
                break
            case Qt.Key_Plus:
            case Qt.Key_Equal:
                viewer.zoomBy(1.25)
                break
            case Qt.Key_Minus:
            case Qt.Key_Underscore:
                viewer.zoomBy(1 / 1.25)
                break
            case Qt.Key_Question:
            case Qt.Key_F1:
                viewer.showShortcutHelp()
                break
            case Qt.Key_W:
                if (!video && shift) viewer.toggleFill()
                else if (!video) viewer.setFitMode("width")
                break
            case Qt.Key_0:
                if (!video) viewer.fitToWindow()
                break
            case Qt.Key_1:
                if (!video) viewer.toggleActualSize()
                break
            case Qt.Key_R:
                viewer.rotate(!shift)
                break
            case Qt.Key_F:
            case Qt.Key_F11:
                viewer.setFullScreen(!viewer.fullScreen)
                break
            case Qt.Key_F5:
                viewer.setSlideshow(!viewer.slideshowRunning)
                break
            case Qt.Key_I:
                viewer.infoOpen = !viewer.infoOpen
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                Session.openInLibrary()
                break
            case Qt.Key_Delete:
                if (shift) {
                    if (!event.isAutoRepeat) viewer.requestPermanentDelete()
                } else {
                    viewer.requestTrash()
                }
                break
            case Qt.Key_V:
                viewer.toggleFavorite()
                break
            case Qt.Key_Y:
                viewer.perform("copy")
                break
            case Qt.Key_S:
                viewer.perform("send")
                break
            case Qt.Key_D:
            case Qt.Key_O:
                if (Session.isRaw) viewer.perform(event.modifiers & Qt.ShiftModifier ? "choose-editor" : "develop")
                else handled = false
                break
            case Qt.Key_A:
                if (!video) viewer.perform("annotate")
                else handled = false
                break
            case Qt.Key_B:
                if (Session.count > 1) viewer.toggleFilmstrip()
                else handled = false
                break
            case Qt.Key_T:
                if (video) viewer.perform("trim")
                else handled = false
                break
            case Qt.Key_G:
                if (video) viewer.perform("frame")
                else handled = false
                break
            case Qt.Key_P:
                if (video) viewer.perform("play")
                else handled = false
                break
            case Qt.Key_Menu:
                viewer.showActionMenu()
                break
            default:
                handled = false
            }
        }
        event.accepted = handled
    }
}
