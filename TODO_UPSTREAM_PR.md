# TODO: Upstream PR/MR Follow-up

Last updated: 2026-02-20

## Current State

- KRDP working branch: `master`
- KRDP local HEAD: `f08e930a4fa6ded1eaa338a93f3b3eee597412e7`
- KRDP GitHub mirror: `git@github.com:westers/krdp.git` (pushed)
- KRDP upstream remote: `https://invent.kde.org/plasma/krdp.git`
- KRDP divergence vs upstream right now: `origin/master...master = 1 behind, 22 ahead`
- Existing MR draft text: `UPSTREAM_MR_DRAFT.md`

- KPipeWire patch repo local HEAD: `e3b65244367e63cb65a72cc5d855fd6fbfb3cbc1`
- KPipeWire patch repo GitHub: `git@github.com:westers/kpipewire-vaapi-fix.git` (pushed)
- Existing MR draft text (patch summary): `/home/westers/dev/rdp/kpipewire-vaapi-fix/UPSTREAM_MR_DRAFT.md`

## Blocker Encountered

Could not open upstream KDE MRs from this shell due missing KDE Invent auth:

- HTTPS push failed with missing username/auth.
- SSH to `invent.kde.org` failed from this host/session.

## Resume Steps: KRDP MR

1. Authenticate to KDE Invent from a shell with working credentials.
2. Rebase local KRDP branch on latest upstream:
   - `cd /home/westers/dev/krdp`
   - `git fetch origin master`
   - `git rebase origin/master`
3. Push rebased branch to your Invent fork (example):
   - `git remote add invent-fork git@invent.kde.org:<invent_user>/krdp.git`
   - `git push invent-fork HEAD:refs/heads/westers/krdp-damage-main`
4. Open MR on Invent from `westers/krdp-damage-main` -> `plasma/krdp:master`.
5. Paste content from `UPSTREAM_MR_DRAFT.md` and update commit list if rebase changed SHAs.

## Resume Steps: KPipeWire MR

Important: `/home/westers/dev/rdp/kpipewire-vaapi-fix` is a patch-packaging repo, not a branch on top of upstream `plasma/kpipewire`.

Use it as source material for a real upstream branch:

1. Clone your Invent fork of `kpipewire` and branch from upstream `master`.
2. Apply these patches in order from this repo:
   - `/home/westers/dev/rdp/kpipewire-vaapi-fix/patches/01-fix-vaapi-hw-frames-ctx.patch`
   - `/home/westers/dev/rdp/kpipewire-vaapi-fix/patches/02-add-color-range-support.patch`
   - `/home/westers/dev/rdp/kpipewire-vaapi-fix/patches/03-fix-software-encoder-filter-graph.patch`
   - `/home/westers/dev/rdp/kpipewire-vaapi-fix/patches/04-damage-metadata-encoded-stream.patch`
   - `/home/westers/dev/rdp/kpipewire-vaapi-fix/patches/05-honor-h264-profile-in-libx264-fallback.patch`
3. Convert into clean upstream commits, build/test, then push to your Invent fork.
4. Open MR against `plasma/kpipewire:master`.
5. Use `/home/westers/dev/rdp/kpipewire-vaapi-fix/UPSTREAM_MR_DRAFT.md` as base text.

## Validation Commands to Re-run Before Opening MRs

- KRDP:
  - `cd /home/westers/dev/krdp && ./smoke-test.sh --no-build --watch-seconds 120`
- KPipeWire patch install + log watch:
  - `cd /home/westers/dev/rdp/kpipewire-vaapi-fix && ./smoke-test.sh --no-build --force-libx264 --watch-seconds 120`

