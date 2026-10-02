# Farside settings: KDE layout conventions

Steve requested restoring Claude's earlier KDE presentation on October 1, 2026.
Use `e0ba4b77` as the visual precedent, retaining the current Console/Virtual
models and administrator/account boundaries. Do not restore the obsolete
per-user service controls or legacy configuration adapters.

## Layout

- A short opening page: Console and Virtual status, enabled controls, and
  links to their settings. Stopping requires confirmation and targets only
  the selected service; the controls reflect actual state after cancellation.
- Native `KCM.SimpleKCM`, `Kirigami.FormLayout`, section labels, Qt controls,
  system fonts, theme colors, and Kirigami spacing.
- Short descriptions and adjacent `ContextualHelpButton` explanations.
  Show errors and pending application notices only when relevant. Keep essential
  consequences visible, including restart/disconnect and password preservation.
- Align field labels beside controls on wide windows, and let FormLayout put
  labels above controls at narrow widths. Bound long fingerprints and technical
  text so they do not force every form into the narrow layout.
- Quality offers a slider and editable numeric value. Opening or refreshing
  the page must not snap or change the saved value.
- Keep all host/session fields and account preferences available in their scope.
  Field inheritance, locked preferences, unavailable Virtual camera, certificate
  operations, drafts, validation, and runtime inspection retain their model behavior.
- Load/save remain explicit and scoped. Saving settings never restarts a service.
  Services use descriptive Stop/Restart confirmations and separate startup controls.

Host form objects stay alive during scope switches. Identify fields by both key
and group: VA-API policy occurs in different groups across host/session schemas.
Avoid transient repeated forms linked as twins; Kirigami's deferred twin/label
updates can otherwise run after those objects have been destroyed.

## KDE references reviewed

- [Human Interface Guidelines](https://develop.kde.org/hig/)
- [Simple by default](https://develop.kde.org/hig/simple_by_default/)
- [Layout and navigation](https://develop.kde.org/hig/layout_and_nav/)
- [Displaying content and inline help](https://develop.kde.org/hig/displaying_content/)
- [Getting input](https://develop.kde.org/hig/getting_input/)
- [Status changes](https://develop.kde.org/hig/status_changes/)
- [Text and labels](https://develop.kde.org/hig/text_and_labels/)
- [Accessibility](https://develop.kde.org/hig/accessibility/)
- [Kirigami form layouts](https://develop.kde.org/docs/getting-started/kirigami/components-formlayouts/)
- [KCM development](https://develop.kde.org/docs/features/configuration/kcm/)

Use the existing scoped widget/KCM fixtures and visually inspect their rendered
pages on Buzz. Actual administrator-dialog acceptance remains the separate N04
manual check; UI fixture success does not substitute for it. No Hal live testing.
