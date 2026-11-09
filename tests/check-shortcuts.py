#!/usr/bin/env python3
"""Keep the shared table and the quick/grid handlers in agreement."""
import json
import pathlib
import re

root = pathlib.Path(__file__).resolve().parent.parent
source = (root / 'src/app/ShortcutTable.h').read_text()
rows = json.loads(source.split('R"shortcuts(', 1)[1].split(')shortcuts"', 1)[0])
names = {'+': 'Plus', '=': 'Equal', '-': 'Minus', '_': 'Underscore', ',': 'Comma', '.': 'Period', '[': 'BracketLeft', ']': 'BracketRight', '?': 'Question', 'Esc': 'Escape', 'Del': 'Delete', 'PgUp': 'PageUp', 'PgDown': 'PageDown'}
for context, file, implicit in [('quick', 'ViewerKeys.qml', {'2', '3', '4'}), ('library', 'CaptureGrid.qml', set())]:
    handler = (root / 'qml/components' / file).read_text()
    assert not re.search(r"Qt\.Key_", handler), "Shortcut key defined outside the table"
    actual = set(re.findall(rf'ShortcutCatalog\.{context}Keys\.(\w+)', handler))
    actual.update(re.findall(rf'ShortcutCatalog\.{context}Keys\["(\d+)"\]', handler))
    documented = set()
    for row in rows:
        assert row['label'] and row['group'] and row['keys'], row
        assert len(row['tokens']) == len(row['keys']), row
        if row['context'] != context:
            continue
        for index, key in enumerate(row['keys']):
            if len(row['tokens']) > index:
                documented.add(row['tokens'][index])
            else:
                token = '+' if key.endswith('+') else key.rsplit('+', 1)[-1]
                documented.add(names.get(token, token))
    assert actual <= documented, actual - documented
    # Library action handlers live in Main and registry, not CaptureGrid.
    if context == 'library':
        main = (root / 'qml/Main.qml').read_text()
        assert not re.search(r'sequences?:\s*\[?"', main), "Library shortcut defined outside the table"
        registry = (root / 'src/actions/ActionRegistry.cpp').read_text()
        ids = set(re.findall(r'ShortcutCatalog.sequence\("library", "([^"]+)"\)', main))
        ids.update(re.findall(r'ShortcutTable::hint\(QStringLiteral\("library"\), QStringLiteral\("([^"]+)"\)\)', registry))
        table_ids = {r['id'] for r in rows if r['context'] == 'library'}
        assert ids <= table_ids, ids - table_ids
        grid_ids = {'details', 'primary', 'select', 'left', 'right', 'up', 'down', 'ends', 'section-next', 'section-prev'}
        assert table_ids <= ids | grid_ids, table_ids - ids - grid_ids
    else:
        assert actual <= documented, actual - documented
        assert documented <= actual | implicit, documented - actual - implicit
print('Shortcut coverage passed (quick viewer and library, including reverse coverage)')
