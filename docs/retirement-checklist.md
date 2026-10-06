# Retirement checklist (route, unit or feature)

Written 2026-10-05 after the :3389 retirement removed "replace my screens" without anyone listing what would
stop working (`docs/issues/2026-10-05-console-replace-screens-analysis.md`, section 8).

Fill this in **before** disabling any route, unit or feature. If any row is open, do not disable it: tell Steve
which behaviours will stop and let him decide.

1. List every user-visible behaviour of the thing being retired. Sources: the consolidation inventory
   (`docs/superpowers/specs/2026-09-29-console-virtual-consolidation-design.md`), the plan rows that mention it,
   and the last 30 days of its journal (for example the log line `Layout action ... CreateStandIn`).
2. One row per behaviour:

   | Behaviour | Replacement | Owner | Passing test on the target host | Steve: yes / no |
   |---|---|---|---|---|

3. A row is closed only when the replacement is implemented, a test passed on the real target host (not only a
   fixture), and Steve answered yes or no.
4. A UI label that is kept across routes must keep its meaning, or be renamed.
5. Acceptance must check that user-visible behaviours still work, not only that the replacement connects and
   streams.
6. Record the finished checklist in the HANDOFF Log with the retirement.
