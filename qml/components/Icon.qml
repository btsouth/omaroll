import QtQuick
import QtQuick.Shapes

// A line icon drawn from a path on a 24 unit grid, so it stays crisp at any
// scale and takes its colour from the theme rather than from a bitmap.
Item {
    id: root

    property string name: ""
    property color color: Theme.foreground
    property real size: 18

    implicitWidth: size
    implicitHeight: size

    // Outlines, stroked at two units.
    readonly property var strokes: ({
        "chevron-left": "M15 5 L8 12 L15 19",
        "chevron-right": "M9 5 L16 12 L9 19",
        "info": "M12 3 A9 9 0 1 1 12 21 A9 9 0 1 1 12 3 Z M12 11 L12 16.5 M12 7.6 L12 7.7",
        "grid": "M4 4 L10 4 L10 10 L4 10 Z M14 4 L20 4 L20 10 L14 10 Z "
                + "M4 14 L10 14 L10 20 L4 20 Z M14 14 L20 14 L20 20 L14 20 Z",
        "fullscreen": "M4 9 L4 4 L9 4 M15 4 L20 4 L20 9 M20 15 L20 20 L15 20 M9 20 L4 20 L4 15",
        "fullscreen-exit": "M9 4 L9 9 L4 9 M20 9 L15 9 L15 4 M15 20 L15 15 L20 15 M4 15 L9 15 L9 20",
        "volume": "M4 9.5 L7.5 9.5 L12 5.5 L12 18.5 L7.5 14.5 L4 14.5 Z "
                  + "M15.5 9 A4.2 4.2 0 0 1 15.5 15 M18.2 6.4 A7.8 7.8 0 0 1 18.2 17.6",
        "volume-muted": "M4 9.5 L7.5 9.5 L12 5.5 L12 18.5 L7.5 14.5 L4 14.5 Z "
                        + "M16 9.5 L21 14.5 M21 9.5 L16 14.5",
        "star": "M12 3.5 L14.6 8.9 L20.5 9.6 L16.1 13.6 L17.3 19.5 L12 16.6 L6.7 19.5 "
                + "L7.9 13.6 L3.5 9.6 L9.4 8.9 Z",
        "captions": "M3.5 6 L20.5 6 L20.5 18 L3.5 18 Z M10.6 10.2 A2.3 2.3 0 1 0 10.6 13.8 "
                    + "M16.6 10.2 A2.3 2.3 0 1 0 16.6 13.8",
        "close": "M6 6 L18 18 M18 6 L6 18"
    })
    // Solid shapes.
    readonly property var fills: ({
        "play": "M8 5.2 L19 12 L8 18.8 Z",
        "pause": "M6.5 5 L10 5 L10 19 L6.5 19 Z M14 5 L17.5 5 L17.5 19 L14 19 Z",
        "more": "M5 12 A1.7 1.7 0 1 0 8.4 12 A1.7 1.7 0 1 0 5 12 Z "
                + "M10.3 12 A1.7 1.7 0 1 0 13.7 12 A1.7 1.7 0 1 0 10.3 12 Z "
                + "M15.6 12 A1.7 1.7 0 1 0 19 12 A1.7 1.7 0 1 0 15.6 12 Z",
        "star-filled": "M12 3.5 L14.6 8.9 L20.5 9.6 L16.1 13.6 L17.3 19.5 L12 16.6 L6.7 19.5 "
                       + "L7.9 13.6 L3.5 9.6 L9.4 8.9 Z"
    })

    Shape {
        width: 24
        height: 24
        scale: root.size / 24
        transformOrigin: Item.TopLeft
        preferredRendererType: Shape.CurveRenderer
        antialiasing: true

        ShapePath {
            strokeColor: root.strokes[root.name] !== undefined ? root.color : "transparent"
            strokeWidth: 2
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: root.strokes[root.name] !== undefined ? root.strokes[root.name] : "" }
        }
        ShapePath {
            strokeColor: "transparent"
            fillColor: root.fills[root.name] !== undefined ? root.color : "transparent"
            PathSvg { path: root.fills[root.name] !== undefined ? root.fills[root.name] : "" }
        }
    }
}
