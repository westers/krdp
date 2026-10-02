# Settings redesign implementation review

The user approved the October 1 overview/subpage proposal and requested implementation.
This review covers the eight changed/new Broker QML components and their draft models.
KDE theme, Kirigami units, i18nc and persistent scoped editors are the project conventions.

## Functional results

- Ordinary host defaults preserve TLS paths, explicit TLS operation and imported PEM.
- A certificate editor owns a separate C++ draft. Cancel/Back preserve all parent changes;
  accepting copies only TLS fields. Child helper operations are blocked. Changed parent
  revisions reject staging with a recovery message. PEM remains in C++, with bounded
  inspection and existing secure helper validation.
- Account Custom starts incomplete; both fallback dimensions must be chosen. Hidden
  display options remain staged/preserved. Locks and unrelated configuration are preserved.
- Access removal has local Undo; Revert restores the loaded snapshot. Password dialogs
  require a new password for a new alias or changed owner and clear it on dismissal.
- Immediate service actions and scoped saves remain separate. Saved wildcard bindings
  are not offered as network addresses. Runtime inspection states moved to service details.

## Six focused review passes

Performed by the current agent, following the user's task-by-task process; no review fanout.

| Domain | Reviewed result |
|---|---|
| Bindings | Edited control values rebind after interaction; service events do not recreate editors; explicit numeric and address signal arguments |
| Layout | Bounded content and persistent twin forms; removed circular parent-width maximum constraints; settled native narrow footers verified |
| Loading/lifecycle | Static pages retain drafts/focus/scroll; local certificate draft clears on Back/Cancel; stale picker acceptance ignored |
| Delegates | Static host/preference definitions; required roles and scoped IDs; conditional hardware inventory; no service-state model churn |
| State | Scoped load/save/revert/default/reload and confirmed stop/restart; incomplete numeric drafts and alias passwords do not enable unsafe saves |
| Performance | No periodic polls, nested TLS modal or model reconstruction from service status; native controls/software fixture renders |

The deterministic scanner and Qt6 qmllint were run and retained in the evidence folder.
Their generic ordering/property-var/style and context/type warnings are not a claim of
confirmed functional bugs; KDE's injected i18nc/KCM model context is not fully known to
standalone qmllint. Native fixture checks and the compiled KCM plugin provide runtime evidence.

## Acceptance limits

Fixture authorization/cancellation/readback do not prove the real KDE administrator dialog.
Native administrator Save/restart and user visual acceptance remain manual N04 checks.
No wallet operations, Hal GUI/media test or performance benchmark is part of this delivery.
