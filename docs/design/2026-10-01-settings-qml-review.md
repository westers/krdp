## QML Code Review Report

**Scope**: files: `src/kcm/ui/*.qml`, with supporting C++ model traces
**Files reviewed**: 10 QML files
**Issues found**: 151 review items (144 scanner findings, 7 deep analysis findings). Scanner findings are not 144 confirmed functional bugs.
**qmllint**: ran Qt 6 `/usr/lib/qt6/bin/qmllint`; 140 warnings plus 1 unused-import information diagnostic. Three files clean; seven have diagnostics. Not a clean lint result.

No product edits, live GUI tests or deployments. Six focused analysis passes were completed by one agent under the standing no-delegation rule. Raw scanner and Qt 6 JSON are archived in `~/dev/rdp/evidence/2026-10-01-settings-design-review/`. Qt 6 diagnostics are preserved there; many need the compiled KCM type/translation context. The earlier `/usr/bin/qmllint` attempt rejected `--json` and was replaced with Qt 6, not counted as successful.

The 144 scanner findings below are unfiltered. Qt 6 separately reports 138 unqualified-access warnings, two Dialog layout-position warnings overlapping the scanner, and one unused import. These are not added to the scanner total as though they were independent functional defects.

---

### Lint findings

#### [L-001] BND-1 — BrokerHostsPage.qml:14
- **File**: `src/kcm/ui/BrokerHostsPage.qml:14`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-002] ORD-1 — BrokerHostsPage.qml:14
- **File**: `src/kcm/ui/BrokerHostsPage.qml:14`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-003] BND-1 — BrokerHostsPage.qml:15
- **File**: `src/kcm/ui/BrokerHostsPage.qml:15`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-004] BND-1 — BrokerHostsPage.qml:16
- **File**: `src/kcm/ui/BrokerHostsPage.qml:16`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-005] BND-1 — BrokerHostsPage.qml:24
- **File**: `src/kcm/ui/BrokerHostsPage.qml:24`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-006] BND-1 — BrokerHostsPage.qml:25
- **File**: `src/kcm/ui/BrokerHostsPage.qml:25`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-007] ORD-1 — BrokerHostsPage.qml:107
- **File**: `src/kcm/ui/BrokerHostsPage.qml:107`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-008] ORD-1 — BrokerHostsPage.qml:112
- **File**: `src/kcm/ui/BrokerHostsPage.qml:112`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-009] ORD-1 — BrokerHostsPage.qml:121
- **File**: `src/kcm/ui/BrokerHostsPage.qml:121`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-010] ORD-1 — BrokerHostsPage.qml:133
- **File**: `src/kcm/ui/BrokerHostsPage.qml:133`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-011] ORD-1 — BrokerHostsPage.qml:139
- **File**: `src/kcm/ui/BrokerHostsPage.qml:139`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-012] JS-2 — BrokerHostsPage.qml:164
- **File**: `src/kcm/ui/BrokerHostsPage.qml:164`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-013] ORD-1 — BrokerHostsPage.qml:164
- **File**: `src/kcm/ui/BrokerHostsPage.qml:164`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-014] ORD-1 — BrokerHostsPage.qml:171
- **File**: `src/kcm/ui/BrokerHostsPage.qml:171`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-015] ORD-1 — BrokerHostsPage.qml:188
- **File**: `src/kcm/ui/BrokerHostsPage.qml:188`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-016] ORD-1 — BrokerHostsPage.qml:196
- **File**: `src/kcm/ui/BrokerHostsPage.qml:196`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-017] JS-2 — BrokerHostsPage.qml:197
- **File**: `src/kcm/ui/BrokerHostsPage.qml:197`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-018] ORD-1 — BrokerHostsPage.qml:203
- **File**: `src/kcm/ui/BrokerHostsPage.qml:203`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-019] JS-2 — BrokerHostsPage.qml:204
- **File**: `src/kcm/ui/BrokerHostsPage.qml:204`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-020] ORD-1 — BrokerHostsPage.qml:224
- **File**: `src/kcm/ui/BrokerHostsPage.qml:224`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-021] ORD-1 — BrokerHostsPage.qml:235
- **File**: `src/kcm/ui/BrokerHostsPage.qml:235`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-022] ORD-1 — BrokerHostsPage.qml:249
- **File**: `src/kcm/ui/BrokerHostsPage.qml:249`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-023] JS-2 — BrokerHostsPage.qml:263
- **File**: `src/kcm/ui/BrokerHostsPage.qml:263`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-024] ORD-1 — BrokerHostsPage.qml:269
- **File**: `src/kcm/ui/BrokerHostsPage.qml:269`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-025] ORD-1 — BrokerHostsPage.qml:275
- **File**: `src/kcm/ui/BrokerHostsPage.qml:275`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-026] ORD-1 — BrokerHostsPage.qml:283
- **File**: `src/kcm/ui/BrokerHostsPage.qml:283`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-027] JS-2 — BrokerHostsPage.qml:325
- **File**: `src/kcm/ui/BrokerHostsPage.qml:325`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-028] ORD-1 — BrokerHostsPage.qml:325
- **File**: `src/kcm/ui/BrokerHostsPage.qml:325`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-029] ORD-1 — BrokerHostsPage.qml:330
- **File**: `src/kcm/ui/BrokerHostsPage.qml:330`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-030] ORD-1 — BrokerHostsPage.qml:335
- **File**: `src/kcm/ui/BrokerHostsPage.qml:335`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-031] JS-2 — BrokerHostsPage.qml:341
- **File**: `src/kcm/ui/BrokerHostsPage.qml:341`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-032] ORD-1 — BrokerHostsPage.qml:341
- **File**: `src/kcm/ui/BrokerHostsPage.qml:341`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-033] ORD-1 — BrokerHostsPage.qml:348
- **File**: `src/kcm/ui/BrokerHostsPage.qml:348`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-034] ORD-1 — BrokerHostsPage.qml:355
- **File**: `src/kcm/ui/BrokerHostsPage.qml:355`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-035] ORD-1 — BrokerHostsPage.qml:362
- **File**: `src/kcm/ui/BrokerHostsPage.qml:362`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-036] ORD-1 — BrokerHostsPage.qml:371
- **File**: `src/kcm/ui/BrokerHostsPage.qml:371`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-037] ORD-1 — BrokerHostsPage.qml:378
- **File**: `src/kcm/ui/BrokerHostsPage.qml:378`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-038] ORD-1 — BrokerHostsPage.qml:386
- **File**: `src/kcm/ui/BrokerHostsPage.qml:386`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-039] BND-1 — BrokerHostsPage.qml:397
- **File**: `src/kcm/ui/BrokerHostsPage.qml:397`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-040] ORD-1 — BrokerHostsPage.qml:397
- **File**: `src/kcm/ui/BrokerHostsPage.qml:397`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-041] BND-1 — BrokerHostsPage.qml:410
- **File**: `src/kcm/ui/BrokerHostsPage.qml:410`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-042] ORD-1 — BrokerHostsPage.qml:410
- **File**: `src/kcm/ui/BrokerHostsPage.qml:410`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-043] BND-1 — BrokerHostsPage.qml:421
- **File**: `src/kcm/ui/BrokerHostsPage.qml:421`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-044] JS-2 — BrokerHostsPage.qml:425
- **File**: `src/kcm/ui/BrokerHostsPage.qml:425`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-045] ORD-1 — BrokerHostsPage.qml:433
- **File**: `src/kcm/ui/BrokerHostsPage.qml:433`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-046] BND-1 — BrokerHostsPage.qml:480
- **File**: `src/kcm/ui/BrokerHostsPage.qml:480`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-047] ORD-1 — BrokerHostsPage.qml:480
- **File**: `src/kcm/ui/BrokerHostsPage.qml:480`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-048] BND-1 — BrokerHostsPage.qml:493
- **File**: `src/kcm/ui/BrokerHostsPage.qml:493`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-049] ORD-1 — BrokerHostsPage.qml:493
- **File**: `src/kcm/ui/BrokerHostsPage.qml:493`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-050] BND-1 — BrokerHostsPage.qml:501
- **File**: `src/kcm/ui/BrokerHostsPage.qml:501`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-051] ORD-1 — BrokerHostsPage.qml:501
- **File**: `src/kcm/ui/BrokerHostsPage.qml:501`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-052] BND-1 — BrokerMainPage.qml:14
- **File**: `src/kcm/ui/BrokerMainPage.qml:14`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-053] ORD-1 — BrokerMainPage.qml:14
- **File**: `src/kcm/ui/BrokerMainPage.qml:14`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-054] BND-1 — BrokerMainPage.qml:15
- **File**: `src/kcm/ui/BrokerMainPage.qml:15`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-055] BND-1 — BrokerMainPage.qml:16
- **File**: `src/kcm/ui/BrokerMainPage.qml:16`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-056] BND-1 — BrokerMainPage.qml:17
- **File**: `src/kcm/ui/BrokerMainPage.qml:17`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-057] BND-1 — BrokerMainPage.qml:18
- **File**: `src/kcm/ui/BrokerMainPage.qml:18`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-058] BND-1 — BrokerMainPage.qml:19
- **File**: `src/kcm/ui/BrokerMainPage.qml:19`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-059] BND-1 — BrokerMainPage.qml:20
- **File**: `src/kcm/ui/BrokerMainPage.qml:20`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-060] ORD-1 — BrokerMainPage.qml:29
- **File**: `src/kcm/ui/BrokerMainPage.qml:29`
- **Rule**: ORD-1
- **Finding**: id appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-061] ORD-1 — BrokerMainPage.qml:44
- **File**: `src/kcm/ui/BrokerMainPage.qml:44`
- **Rule**: ORD-1
- **Finding**: id appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-062] ORD-1 — BrokerMainPage.qml:57
- **File**: `src/kcm/ui/BrokerMainPage.qml:57`
- **Rule**: ORD-1
- **Finding**: id appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-063] ORD-1 — BrokerMainPage.qml:70
- **File**: `src/kcm/ui/BrokerMainPage.qml:70`
- **Rule**: ORD-1
- **Finding**: id appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-064] BND-1 — BrokerPreferencesPage.qml:13
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:13`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-065] ORD-1 — BrokerPreferencesPage.qml:13
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:13`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-066] ORD-1 — BrokerPreferencesPage.qml:21
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:21`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-067] JS-2 — BrokerPreferencesPage.qml:27
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:27`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-068] ORD-1 — BrokerPreferencesPage.qml:27
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:27`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-069] ORD-1 — BrokerPreferencesPage.qml:34
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:34`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-070] ORD-1 — BrokerPreferencesPage.qml:46
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:46`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-071] BND-1 — BrokerPreferencesPage.qml:75
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:75`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-072] ORD-1 — BrokerPreferencesPage.qml:86
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:86`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-073] BND-1 — BrokerServiceControls.qml:10
- **File**: `src/kcm/ui/BrokerServiceControls.qml:10`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-074] BND-1 — BrokerServiceControls.qml:12
- **File**: `src/kcm/ui/BrokerServiceControls.qml:12`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-075] BND-1 — BrokerServiceControls.qml:13
- **File**: `src/kcm/ui/BrokerServiceControls.qml:13`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-076] ORD-1 — BrokerServiceControls.qml:26
- **File**: `src/kcm/ui/BrokerServiceControls.qml:26`
- **Rule**: ORD-1
- **Finding**: child object appears after function -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-077] BND-2 — BrokerServiceControls.qml:39
- **File**: `src/kcm/ui/BrokerServiceControls.qml:39`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'checked' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-078] JS-2 — BrokerServiceControls.qml:50
- **File**: `src/kcm/ui/BrokerServiceControls.qml:50`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-079] ORD-1 — BrokerServiceControls.qml:55
- **File**: `src/kcm/ui/BrokerServiceControls.qml:55`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-080] ORD-1 — BrokerServiceControls.qml:71
- **File**: `src/kcm/ui/BrokerServiceControls.qml:71`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-081] JS-2 — BrokerServiceControls.qml:72
- **File**: `src/kcm/ui/BrokerServiceControls.qml:72`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-082] ORD-1 — BrokerServiceControls.qml:78
- **File**: `src/kcm/ui/BrokerServiceControls.qml:78`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-083] JS-2 — BrokerServiceControls.qml:79
- **File**: `src/kcm/ui/BrokerServiceControls.qml:79`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-084] BND-2 — BrokerServiceControls.qml:101
- **File**: `src/kcm/ui/BrokerServiceControls.qml:101`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'checked' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-085] ORD-1 — BrokerServiceControls.qml:106
- **File**: `src/kcm/ui/BrokerServiceControls.qml:106`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-086] ORD-1 — BrokerServiceControls.qml:125
- **File**: `src/kcm/ui/BrokerServiceControls.qml:125`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-087] LAY-2 — BrokerServiceControls.qml:128
- **File**: `src/kcm/ui/BrokerServiceControls.qml:128`
- **Rule**: LAY-2
- **Finding**: width: inside ColumnLayout child -- use Layout.preferredWidth or Layout.fillWidth
- **Mitigation**: Verify whether this is a layout-managed Item or a Popup. Use layout sizing only for managed Items; do not blindly change Dialog sizing.

#### [L-088] LAY-6 — BrokerServiceControls.qml:129
- **File**: `src/kcm/ui/BrokerServiceControls.qml:129`
- **Rule**: LAY-6
- **Finding**: x: inside ColumnLayout child -- Layout manages positioning; remove explicit x
- **Mitigation**: Verify whether this is a layout-managed Item or a Popup before removing its position.

#### [L-089] STY-1 — BrokerServiceStatus.qml:4
- **File**: `src/kcm/ui/BrokerServiceStatus.qml:4`
- **Rule**: STY-1
- **Finding**: Top-level component should have id: root (enables qualified lookup, future-proofs for QML 3)
- **Mitigation**: Add a meaningful root ID if references need it; small wrapper components do not need a functional rewrite.

#### [L-090] BND-1 — BrokerServiceStatus.qml:5
- **File**: `src/kcm/ui/BrokerServiceStatus.qml:5`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-091] BND-1 — BrokerServicesPage.qml:13
- **File**: `src/kcm/ui/BrokerServicesPage.qml:13`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-092] ORD-1 — BrokerServicesPage.qml:13
- **File**: `src/kcm/ui/BrokerServicesPage.qml:13`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-093] ORD-1 — BrokerServicesPage.qml:17
- **File**: `src/kcm/ui/BrokerServicesPage.qml:17`
- **Rule**: ORD-1
- **Finding**: child object appears after function -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-094] ORD-1 — BrokerServicesPage.qml:28
- **File**: `src/kcm/ui/BrokerServicesPage.qml:28`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-095] ORD-1 — BrokerServicesPage.qml:41
- **File**: `src/kcm/ui/BrokerServicesPage.qml:41`
- **Rule**: ORD-1
- **Finding**: id appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-096] JS-2 — BrokerServicesPage.qml:56
- **File**: `src/kcm/ui/BrokerServicesPage.qml:56`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-097] ORD-1 — BrokerServicesPage.qml:56
- **File**: `src/kcm/ui/BrokerServicesPage.qml:56`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-098] BND-2 — BrokerServicesPage.qml:94
- **File**: `src/kcm/ui/BrokerServicesPage.qml:94`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'checked' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-099] ORD-1 — BrokerServicesPage.qml:99
- **File**: `src/kcm/ui/BrokerServicesPage.qml:99`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-100] ORD-1 — BrokerServicesPage.qml:115
- **File**: `src/kcm/ui/BrokerServicesPage.qml:115`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-101] BND-1 — BrokerSettingField.qml:10
- **File**: `src/kcm/ui/BrokerSettingField.qml:10`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-102] BND-1 — BrokerSettingField.qml:11
- **File**: `src/kcm/ui/BrokerSettingField.qml:11`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-103] ORD-1 — BrokerSettingField.qml:46
- **File**: `src/kcm/ui/BrokerSettingField.qml:46`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-104] BND-2 — BrokerSettingField.qml:65
- **File**: `src/kcm/ui/BrokerSettingField.qml:65`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'checkState' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-105] ORD-1 — BrokerSettingField.qml:75
- **File**: `src/kcm/ui/BrokerSettingField.qml:75`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-106] BND-2 — BrokerSettingField.qml:103
- **File**: `src/kcm/ui/BrokerSettingField.qml:103`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'checked' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-107] ORD-1 — BrokerSettingField.qml:115
- **File**: `src/kcm/ui/BrokerSettingField.qml:115`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-108] ORD-1 — BrokerSettingField.qml:132
- **File**: `src/kcm/ui/BrokerSettingField.qml:132`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-109] JS-2 — BrokerSettingField.qml:136
- **File**: `src/kcm/ui/BrokerSettingField.qml:136`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-110] JS-2 — BrokerSettingField.qml:157
- **File**: `src/kcm/ui/BrokerSettingField.qml:157`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-111] BND-1 — BrokerSignInPage.qml:13
- **File**: `src/kcm/ui/BrokerSignInPage.qml:13`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-112] ORD-1 — BrokerSignInPage.qml:13
- **File**: `src/kcm/ui/BrokerSignInPage.qml:13`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-113] BND-1 — BrokerSignInPage.qml:14
- **File**: `src/kcm/ui/BrokerSignInPage.qml:14`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-114] BND-1 — BrokerSignInPage.qml:15
- **File**: `src/kcm/ui/BrokerSignInPage.qml:15`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-115] BND-1 — BrokerSignInPage.qml:16
- **File**: `src/kcm/ui/BrokerSignInPage.qml:16`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-116] BND-1 — BrokerSignInPage.qml:17
- **File**: `src/kcm/ui/BrokerSignInPage.qml:17`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-117] JS-2 — BrokerSignInPage.qml:21
- **File**: `src/kcm/ui/BrokerSignInPage.qml:21`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-118] BND-2 — BrokerSignInPage.qml:22
- **File**: `src/kcm/ui/BrokerSignInPage.qml:22`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'aliasName.text' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-119] BND-2 — BrokerSignInPage.qml:23
- **File**: `src/kcm/ui/BrokerSignInPage.qml:23`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'ownerName.text' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-120] BND-2 — BrokerSignInPage.qml:24
- **File**: `src/kcm/ui/BrokerSignInPage.qml:24`
- **Rule**: BND-2
- **Finding**: Imperative '=' on 'aliasPassword.text' destroys its binding -- use Qt.binding() to restore, or verify this is intentional
- **Mitigation**: Confirm initialization versus a live binding; restore live bindings with Qt.binding(). Several flagged handlers already restore them immediately.

#### [L-121] ORD-1 — BrokerSignInPage.qml:32
- **File**: `src/kcm/ui/BrokerSignInPage.qml:32`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-122] JS-2 — BrokerSignInPage.qml:38
- **File**: `src/kcm/ui/BrokerSignInPage.qml:38`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-123] ORD-1 — BrokerSignInPage.qml:38
- **File**: `src/kcm/ui/BrokerSignInPage.qml:38`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-124] ORD-1 — BrokerSignInPage.qml:45
- **File**: `src/kcm/ui/BrokerSignInPage.qml:45`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-125] JS-2 — BrokerSignInPage.qml:59
- **File**: `src/kcm/ui/BrokerSignInPage.qml:59`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-126] ORD-1 — BrokerSignInPage.qml:59
- **File**: `src/kcm/ui/BrokerSignInPage.qml:59`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-127] ORD-1 — BrokerSignInPage.qml:64
- **File**: `src/kcm/ui/BrokerSignInPage.qml:64`
- **Rule**: ORD-1
- **Finding**: id appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-128] ORD-1 — BrokerSignInPage.qml:82
- **File**: `src/kcm/ui/BrokerSignInPage.qml:82`
- **Rule**: ORD-1
- **Finding**: id appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-129] ORD-1 — BrokerSignInPage.qml:100
- **File**: `src/kcm/ui/BrokerSignInPage.qml:100`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-130] JS-2 — BrokerSignInPage.qml:118
- **File**: `src/kcm/ui/BrokerSignInPage.qml:118`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-131] ORD-1 — BrokerSignInPage.qml:123
- **File**: `src/kcm/ui/BrokerSignInPage.qml:123`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-132] ORD-1 — BrokerSignInPage.qml:130
- **File**: `src/kcm/ui/BrokerSignInPage.qml:130`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-133] PRF-3 — BrokerSignInPage.qml:132
- **File**: `src/kcm/ui/BrokerSignInPage.qml:132`
- **Rule**: PRF-3
- **Finding**: clip: true disables scene graph batching -- verify this is needed (acceptable on ListView)
- **Mitigation**: Retain clipping where the ListView requires it; assess batching only if a measured rendering problem exists.

#### [L-134] ORD-1 — BrokerSignInPage.qml:135
- **File**: `src/kcm/ui/BrokerSignInPage.qml:135`
- **Rule**: ORD-1
- **Finding**: id appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-135] BND-1 — BrokerSignInPage.qml:136
- **File**: `src/kcm/ui/BrokerSignInPage.qml:136`
- **Rule**: BND-1
- **Finding**: property var -- use a typed property (int, string, etc.) for qmlsc compilation and type safety
- **Mitigation**: Use a registered model/object type where practical; retain deliberate test injection and QVariant collections where a typed replacement is not available.

#### [L-136] ORD-1 — BrokerSignInPage.qml:180
- **File**: `src/kcm/ui/BrokerSignInPage.qml:180`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-137] ORD-1 — BrokerSignInPage.qml:187
- **File**: `src/kcm/ui/BrokerSignInPage.qml:187`
- **Rule**: ORD-1
- **Finding**: id appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-138] JS-2 — BrokerSignInPage.qml:188
- **File**: `src/kcm/ui/BrokerSignInPage.qml:188`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-139] ORD-1 — BrokerSignInPage.qml:219
- **File**: `src/kcm/ui/BrokerSignInPage.qml:219`
- **Rule**: ORD-1
- **Finding**: property declaration appears after property assignment -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-140] ORD-1 — BrokerSignInPage.qml:230
- **File**: `src/kcm/ui/BrokerSignInPage.qml:230`
- **Rule**: ORD-1
- **Finding**: property assignment appears after attached property -- expected order: id, properties, signals, assignments, attached, states, transitions, handlers, children, functions
- **Mitigation**: Use the skill's declaration order when touching the component; this is a style finding, not evidence of a runtime failure.

#### [L-141] JS-2 — BrokerSignInPage.qml:234
- **File**: `src/kcm/ui/BrokerSignInPage.qml:234`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-142] JS-2 — BrokerSignInPage.qml:240
- **File**: `src/kcm/ui/BrokerSignInPage.qml:240`
- **Rule**: JS-2
- **Finding**: Loose equality (==/!=) -- use strict equality (===/!==)
- **Mitigation**: Keep the result type explicit; validate existing uses before changing equality semantics.

#### [L-143] STY-1 — main.qml:3
- **File**: `src/kcm/ui/main.qml:3`
- **Rule**: STY-1
- **Finding**: Top-level component should have id: root (enables qualified lookup, future-proofs for QML 3)
- **Mitigation**: Add a meaningful root ID if references need it; small wrapper components do not need a functional rewrite.

#### [L-144] STY-1 — main_phone.qml:3
- **File**: `src/kcm/ui/main_phone.qml:3`
- **Rule**: STY-1
- **Finding**: Top-level component should have id: root (enables qualified lookup, future-proofs for QML 3)
- **Mitigation**: Add a meaningful root ID if references need it; small wrapper components do not need a functional rewrite.

---

### Deep analysis findings

#### [D-001] Numeric customization silently selects the minimum
- **File**: `src/kcm/ui/BrokerSettingField.qml:102`
- **Category**: Bindings & Properties
- **Confidence**: 99/100
- **Finding**: Turning off account numeric inheritance immediately stages Quality=0 or a timing minimum, without selecting a value.
- **Trace**: BrokerSettingField.defaultValue at line 20 is explicitly empty for account preferences; numericValue converts that empty default and falls back to minimum. onClicked writes numericValue when unchecked. Quality minimum is zero.
- **Mitigation**: Use a local incomplete custom-value draft; stage only a deliberately chosen valid number. Keep unknown inherited defaults unknown.

#### [D-002] Ordinary Defaults also resets certificate policy
- **File**: `src/kcm/ui/BrokerHostsPage.qml:442`
- **Category**: States & Structure
- **Confidence**: 99/100
- **Finding**: Host Defaults can change TLS paths hidden on Access when the host is subsequently saved. This does not itself delete certificate files.
- **Trace**: The host footer calls BrokerHostSettings::defaults() at brokerhostsettings.cpp:194. It clears pending settings/import and sets standard TLS mode for Console/Virtual. save() submits that mode with the host values.
- **Mitigation**: Preserve TLS fields, TLS mode and import buffers during ordinary defaults; offer a separately named certificate reset.

#### [D-003] Certificate dialog operates on the entire shared host draft
- **File**: `src/kcm/ui/BrokerSignInPage.qml:185`
- **Category**: Component Loading & Lifecycle
- **Confidence**: 99/100
- **Finding**: Certificate Save includes unrelated pending host values, and Close has no local TLS rollback. A warning exists, but editing remains coupled across pages.
- **Trace**: The Loader injects the same Console/Virtual host QObject into BrokerHostsPage with certificateOnly=true. Its Save calls host.save(); brokerhostsettings.cpp:213 submits the complete pending map. Close has no draft isolation.
- **Mitigation**: Use a host certificate subpage with a local TLS draft, explicit staging and local cancellation; show the staged TLS choice before the single host save.

#### [D-004] Irrelevant display controls remain visible
- **File**: `src/kcm/ui/BrokerPreferencesPage.qml:50`
- **Category**: States & Structure
- **Confidence**: 98/100
- **Finding**: Display index and client-created layout/policy/fallback editors are shown for all capture modes.
- **Trace**: The display Repeater filters by key only. The delegate visible condition at line 81 checks advanced disclosure, not MonitorMode. Followed MonitorMode and dependent definitions in brokerpreferences.cpp.
- **Mitigation**: Conditionally show mode-dependent controls without deleting their inactive values; do not guess an unknown inherited mode.

#### [D-005] Certificate workflow opens dialogs from a modal settings dialog
- **File**: `src/kcm/ui/BrokerHostsPage.qml:253`
- **Category**: States & Structure
- **Confidence**: 97/100
- **Finding**: TLS import opens native certificate/key dialogs while the full certificate settings modal remains open.
- **Trace**: BrokerSignInPage certificateDialog is modal. Its certificateOnly host editor opens certDialog/keyDialog, defined as Dialogs.FileDialog at lines 490/498.
- **Mitigation**: Place certificate editing on a KCM subpage and open only one native file picker at a time.

#### [D-006] Immediate startup action uses a staged-setting control
- **File**: `src/kcm/ui/BrokerServiceControls.qml:93`
- **Category**: States & Structure
- **Confidence**: 99/100
- **Finding**: Start at boot is a checkbox whose click immediately enables/disables the service; it does not wait for host Save.
- **Trace**: onClicked invokes administration.perform(enable/disable), then restores checked from actual service state. KDE HIG Getting Input assigns switches to immediate actions.
- **Mitigation**: Use a stable-label switch with progress, authorization failure and actual readback; retain separate state from host drafts.

#### [D-007] Remote-login staging is enabled before password requirements pass
- **File**: `src/kcm/ui/BrokerSignInPage.qml:240`
- **Category**: States & Structure
- **Confidence**: 99/100
- **Finding**: The dialog permits staging a new login, or a changed owner, with no required new password and then rejects it through a general model error.
- **Trace**: Button enablement checks only nonempty alias/owner. BrokerAuthenticationSettings::setAlias at lines 80–81 rejects missing password for a new alias or changed owner.
- **Mitigation**: Show field-specific requirements before submission and disable Add/Update until valid; use Kirigami.PasswordField and clear transient text on dismissal.

---

### Investigation targets (human verification needed)

#### [I-001] Locale-aware numeric entry
- **File**: `src/kcm/ui/BrokerSettingField.qml:88`
- **Category**: Bindings & Properties
- **Confidence**: 75/100
- **Finding**: String/Number conversion may reject localized digits or grouping in editable numeric fields.
- **Unverified because**: No native Qt session in a non-English locale was run.
- **How to verify**: During the numeric-control implementation, check a supported locale with localized digits/grouping and use locale-aware conversion rather than adding a broad locale suite.

#### [I-002] Service confirmation placement
- **File**: `src/kcm/ui/BrokerServiceControls.qml:128`
- **Category**: Layout & Anchoring
- **Confidence**: 65/100
- **Finding**: The stop/restart Dialog positions x relative to a form column and leaves y to defaults; its intended visual placement in small windows should be checked.
- **Unverified because**: The lint scanners see layout nesting but do not establish Popup positioning semantics or the rendered result. Existing fixture checks are not a full native narrow-dialog visual review.
- **How to verify**: Inspect one stop/restart prompt in the populated native narrow-window review; use Kirigami.PromptDialog if it provides appropriate sizing/placement.

---

### Summary

| Category | Lint | Deep | Investigate | Total |
|---|---:|---:|---:|---:|
| Bindings & Properties | 41 | 1 | 1 | 43 |
| Layout & Anchoring | 2 | 0 | 1 | 3 |
| Component Loading & Lifecycle | 0 | 1 | 0 | 1 |
| ListView & Delegates | 0 | 0 | 0 | 0 |
| States & Structure | 79 | 5 | 0 | 84 |
| Performance & Quality | 22 | 0 | 0 | 22 |
| **Total** | **144** | **7** | **2** | **153** |

The functional findings are source-confirmed, not live user acceptance. The six passes also checked model ownership, persistent pages, stable delegate identity, the removed polling path and rebinding handlers. No new high-confidence defect was established in the current refresh lifetime/performance path. Do not revive the old polling code or turn lint cleanup into a delivery gate.
