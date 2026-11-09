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
        "close": "M6 6 L18 18 M18 6 L6 18",
        "warning": "M12 3.8 L21.2 19.6 L2.8 19.6 Z M12 9.5 L12 14 M12 16.6 L12 16.7",
        "zoom-in": "M10.5 4 A6.5 6.5 0 1 1 10.5 17 A6.5 6.5 0 1 1 10.5 4 Z M15.3 15.3 L20 20 "
                   + "M10.5 7.8 L10.5 13.2 M7.8 10.5 L13.2 10.5",
        "zoom-out": "M10.5 4 A6.5 6.5 0 1 1 10.5 17 A6.5 6.5 0 1 1 10.5 4 Z M15.3 15.3 L20 20 "
                    + "M7.8 10.5 L13.2 10.5",
        "rotate-left": "M4 12 A8 8 0 1 0 12 4 A8.6 8.6 0 0 0 6 6.4 L4 8.4 M4 4 L4 8.4 L8.4 8.4",
        "rotate-right": "M20 12 A8 8 0 1 1 12 4 A8.6 8.6 0 0 1 18 6.4 L20 8.4 M20 4 L20 8.4 L15.6 8.4",
        "flip-horizontal": "M12 3 L12 5 M12 8 L12 10 M12 13 L12 15 M12 18 L12 21 "
                           + "M9 6 L9 18 L3.5 12 Z M15 6 L15 18 L20.5 12 Z",
        "flip-vertical": "M3 12 L5 12 M8 12 L10 12 M13 12 L15 12 M18 12 L21 12 "
                         + "M6 9 L18 9 L12 3.5 Z M6 15 L18 15 L12 20.5 Z",
        "settings": "M10.11 5.16 L10.39 2.64 L13.61 2.64 L13.89 5.16 L15.50 5.82 L17.48 4.24 L19.76 6.52 L18.18 8.50 L18.84 10.11 L21.36 10.39 L21.36 13.61 L18.84 13.89 L18.18 15.50 L19.76 17.48 L17.48 19.76 L15.50 18.18 "
                    + "L13.89 18.84 L13.61 21.36 L10.39 21.36 L10.11 18.84 L8.50 18.18 L6.52 19.76 L4.24 17.48 L5.82 15.50 L5.16 13.89 L2.64 13.61 L2.64 10.39 L5.16 10.11 L5.82 8.50 L4.24 6.52 L6.52 4.24 L8.50 5.82 Z M12 9 A3 3 0 1 1 12 15 A3 3 0 1 1 12 9 Z",
        "search": "M10.5 4 A6.5 6.5 0 1 1 10.5 17 A6.5 6.5 0 1 1 10.5 4 Z M15.3 15.3 L20 20",
        "chevron-down": "M6 9 L12 15 L18 9",
        "save": "M12 4 L12 14 M8 10 L12 14 L16 10 M5 15 L5 19 L19 19 L19 15",
        "trash": "M4.5 6.5 L19.5 6.5 M9.5 6.5 L9.5 4 L14.5 4 L14.5 6.5 M6.5 6.5 L7.4 20 L16.6 20 "
                 + "L17.5 6.5 M10.2 10.5 L10.2 16 M13.8 10.5 L13.8 16"
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
