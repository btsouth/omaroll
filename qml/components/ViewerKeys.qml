import QtQuick

// The quick viewer's keyboard. Each key asks the viewer for one of its own
// actions; video, pictures and animations share keys where that reads well.
Item {
    // The Viewer window whose actions the keys perform.
    required property var viewer

    Keys.onPressed: function (event) {
        if (event.key === ShortcutCatalog.quickKeys.Tab || event.key === ShortcutCatalog.quickKeys.Backtab) viewer.keyboardNavigation = true
        if (viewer.choosingEditor) {
            event.accepted = false
            return
        }
        const control = (event.modifiers & Qt.ControlModifier) !== 0
        const alt = (event.modifiers & Qt.AltModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        const video = Session.isVideo
        let handled = true

        if (video && shift && !control && !alt && event.key === ShortcutCatalog.quickKeys.L) {
            if (!event.isAutoRepeat) viewer.toggleVideoLoop()
        } else if (video && shift && !control && !alt && event.key === ShortcutCatalog.quickKeys.A) {
            viewer.cycleAudioTrack()
        } else if (alt && event.key >= ShortcutCatalog.quickKeys["0"] && event.key <= ShortcutCatalog.quickKeys["5"]) {
            viewer.rate(event.key - ShortcutCatalog.quickKeys["0"])
        } else if (control && event.key === ShortcutCatalog.quickKeys.Z) {
            viewer.undo()
        } else if (control && event.key === ShortcutCatalog.quickKeys.C) {
            if (shift) viewer.copyPath(false)
            else viewer.perform("copy")
        } else if (control && !shift && event.key === ShortcutCatalog.quickKeys.S) {
            if (!event.isAutoRepeat) viewer.saveViewCopy()
        } else if (control && event.key === ShortcutCatalog.quickKeys.O) {
            viewer.perform("open-with")
        } else if (control && event.key === ShortcutCatalog.quickKeys.W) {
            viewer.close()
        } else if (control && event.key === ShortcutCatalog.quickKeys.Q) {
            Qt.quit()
        } else if (shift && !control && !alt && !video
                   && [ShortcutCatalog.quickKeys.Left, ShortcutCatalog.quickKeys.Right, ShortcutCatalog.quickKeys.Up, ShortcutCatalog.quickKeys.Down].indexOf(event.key) >= 0) {
            viewer.panImage(event.key === ShortcutCatalog.quickKeys.Left ? -80 : event.key === ShortcutCatalog.quickKeys.Right ? 80 : 0,
                          event.key === ShortcutCatalog.quickKeys.Up ? -80 : event.key === ShortcutCatalog.quickKeys.Down ? 80 : 0)
        } else if (control || alt) {
            handled = false
        } else {
            switch (event.key) {
            // Through the controls and back to the picture. The controls
            // hand every key they do not use back here, so shortcuts keep
            // working wherever focus is.
            case ShortcutCatalog.quickKeys.Tab:
            case ShortcutCatalog.quickKeys.Backtab:
                viewer.moveFocus(event.key === ShortcutCatalog.quickKeys.Tab)
                break
            case ShortcutCatalog.quickKeys.Escape:
                if (viewer.controlsFocused) forceActiveFocus(Qt.OtherFocusReason)
                else if (viewer.slideshowRunning) viewer.setSlideshow(false)
                else if (viewer.fullScreen) viewer.setFullScreen(false)
                else if (viewer.infoOpen) viewer.infoOpen = false
                else viewer.close()
                break
            case ShortcutCatalog.quickKeys.Left:
            case ShortcutCatalog.quickKeys.Right: {
                const direction = event.key === ShortcutCatalog.quickKeys.Left ? -1 : 1
                if (video) viewer.seek(direction * 5000)
                else viewer.step(direction)
                break
            }
            case ShortcutCatalog.quickKeys.PageUp:
            case ShortcutCatalog.quickKeys.PageDown:
                viewer.step(event.key === ShortcutCatalog.quickKeys.PageUp ? -1 : 1)
                break
            case ShortcutCatalog.quickKeys.Home:
            case ShortcutCatalog.quickKeys.End:
                if (video && viewer.player) {
                    viewer.seekTo(event.key === ShortcutCatalog.quickKeys.Home ? 0 : viewer.player.duration)
                } else {
                    Session.jump(event.key === ShortcutCatalog.quickKeys.Home ? 0 : Session.count - 1)
                }
                break
            case ShortcutCatalog.quickKeys.Space:
            case ShortcutCatalog.quickKeys.K:
                if (video || Session.isAnimated) viewer.togglePlayback()
                else handled = false
                break
            case ShortcutCatalog.quickKeys.J:
            case ShortcutCatalog.quickKeys.L:
                if (video) viewer.seek(event.key === ShortcutCatalog.quickKeys.J ? -10000 : 10000)
                else handled = false
                break
            case ShortcutCatalog.quickKeys.Comma:
            case ShortcutCatalog.quickKeys.Period:
                if (video) viewer.stepFrame(event.key === ShortcutCatalog.quickKeys.Comma ? -1 : 1)
                else handled = false
                break
            case ShortcutCatalog.quickKeys.Up:
            case ShortcutCatalog.quickKeys.Down:
                if (video) viewer.adjustVolume(event.key === ShortcutCatalog.quickKeys.Up ? 0.05 : -0.05)
                else handled = false
                break
            case ShortcutCatalog.quickKeys.M:
                if (video) {
                    Settings.videoMuted = !Settings.videoMuted
                    viewer.say(Settings.videoMuted ? "Muted" : "Sound on")
                } else {
                    handled = false
                }
                break
            case ShortcutCatalog.quickKeys.C:
                if (video) viewer.cycleSubtitles()
                else handled = false
                break
            case ShortcutCatalog.quickKeys.Z:
            case ShortcutCatalog.quickKeys.X:
                if (video) viewer.adjustSubtitleOffset(event.key === ShortcutCatalog.quickKeys.Z ? -500 : 500)
                else handled = false
                break
            case ShortcutCatalog.quickKeys.BracketLeft:
            case ShortcutCatalog.quickKeys.BracketRight:
                if (video && viewer.player) {
                    viewer.setPlaybackRate(viewer.player.playbackRate
                        + (event.key === ShortcutCatalog.quickKeys.BracketLeft ? -0.25 : 0.25))
                } else {
                    handled = false
                }
                break
            case ShortcutCatalog.quickKeys.Backspace:
                if (video && viewer.player) {
                    viewer.setPlaybackRate(1)
                } else {
                    handled = false
                }
                break
            case ShortcutCatalog.quickKeys.Plus:
            case ShortcutCatalog.quickKeys.Equal:
                viewer.zoomBy(1.25)
                break
            case ShortcutCatalog.quickKeys.Minus:
            case ShortcutCatalog.quickKeys.Underscore:
                viewer.zoomBy(1 / 1.25)
                break
            case ShortcutCatalog.quickKeys.Question:
            case ShortcutCatalog.quickKeys.F1:
                viewer.showShortcutHelp()
                break
            case ShortcutCatalog.quickKeys.W:
                if (!video && shift) viewer.toggleFill()
                else if (!video) viewer.setFitMode("width")
                break
            case ShortcutCatalog.quickKeys["0"]:
                if (!video) viewer.fitToWindow()
                break
            case ShortcutCatalog.quickKeys["1"]:
                if (!video) viewer.toggleActualSize()
                break
            case ShortcutCatalog.quickKeys.R:
                viewer.rotate(!shift)
                break
            case ShortcutCatalog.quickKeys.F:
            case ShortcutCatalog.quickKeys.F11:
                viewer.setFullScreen(!viewer.fullScreen)
                break
            case ShortcutCatalog.quickKeys.F5:
                viewer.setSlideshow(!viewer.slideshowRunning)
                break
            case ShortcutCatalog.quickKeys.I:
                viewer.infoOpen = !viewer.infoOpen
                break
            case ShortcutCatalog.quickKeys.Return:
            case ShortcutCatalog.quickKeys.Enter:
                Session.openInLibrary()
                break
            case ShortcutCatalog.quickKeys.Delete:
                if (shift) {
                    if (!event.isAutoRepeat) viewer.requestPermanentDelete()
                } else {
                    viewer.requestTrash()
                }
                break
            case ShortcutCatalog.quickKeys.V:
                if (shift && !video) viewer.flip(false)
                else viewer.toggleFavorite()
                break
            case ShortcutCatalog.quickKeys.H:
                if (shift && !video) viewer.flip(true)
                else handled = false
                break
            case ShortcutCatalog.quickKeys.Y:
                viewer.perform("copy")
                break
            case ShortcutCatalog.quickKeys.S:
                viewer.perform("send")
                break
            case ShortcutCatalog.quickKeys.D:
            case ShortcutCatalog.quickKeys.O:
                if (Session.isRaw) viewer.perform(event.modifiers & Qt.ShiftModifier ? "choose-editor" : "develop")
                else handled = false
                break
            case ShortcutCatalog.quickKeys.A:
                if (!video) viewer.perform("annotate")
                else handled = false
                break
            case ShortcutCatalog.quickKeys.B:
                if (Session.count > 1) viewer.toggleFilmstrip()
                else handled = false
                break
            case ShortcutCatalog.quickKeys.T:
                if (video) viewer.perform("trim")
                else handled = false
                break
            case ShortcutCatalog.quickKeys.G:
                if (video) viewer.perform("frame")
                else handled = false
                break
            case ShortcutCatalog.quickKeys.P:
                if (video) viewer.perform("play")
                else handled = false
                break
            case ShortcutCatalog.quickKeys.Menu:
                viewer.showActionMenu()
                break
            default:
                handled = false
            }
        }
        event.accepted = handled
    }
}
