# Explicit virtual-service staging (not production enablement)

`KRdpVirtualSessionTest` is an opt-in CMake install component. It contains the
broker, lifecycle helpers, guardian, device entry, capture worker, KRdp library,
launcher scripts, support configuration and inactive unit/PAM drafts. Ordinary
installs exclude these extra rules. No rule installs a virtual unit into systemd
or a policy into `/etc/pam.d`, enables a service, or starts a process.

Use a separate build directory and configure the intended final prefix BEFORE
building: the draft units embed `CMAKE_INSTALL_PREFIX`; changing only
`cmake --install --prefix` does not rewrite them. Keep the prefix distinct from
the physical console installation and every prefix currently used by a process.
Use the expected `bin` and `share` install directories: draft unit suffixes are
fixed even though KDE's install-directory variables are configurable.
For an already configured/built directory, unprivileged staging is:

```sh
task_stage=$(mktemp -d /tmp/krdp-virtual-stage.XXXXXX)
DESTDIR="$task_stage" cmake --install build-virtual-test --component KRdpVirtualSessionTest
```

This stages project artifacts only, NOT a root-ready dependency closure. Before
an administrator installs or runs them:

- Stage the matching private KPipeWire libraries in the separate prefix. Never
  replace a mapped library in the physical or retained-test installations.
- Configure install runtime paths to the final root-owned library directory;
  disable automatic inclusion of build/link directories. Inspect every ELF
  runtime path and resolved dependency. Build-tree `/home/...` paths are not
  acceptable for root services. Empty paths can also select incompatible distro
  KRdp/KPipeWire libraries; successful copying alone is not link acceptance.
- Verify canonical paths and root ownership/non-writability of code, libraries,
  support files and every ancestor after the administrator copies the stage.
  For the fixed `/opt/krdp-virtual-service-test` acceptance prefix, run
  `bash scripts/check-virtual-session-install.sh` as the ordinary account.
  It checks ownership, all twelve required ELF runtime paths and clean-environment
  library/symbol resolution without starting services. Run only while the
  administrator is not changing the installation; it is not a race-proof sandbox
  or an artifact-authenticity check. It intentionally refuses root invocation.
- Review unit paths, renderer allow-list, PAM policy including common-account,
  TLS ownership and journal permissions. Install drafts only as a separate
  explicit administrative step, never through the ordinary install component.
- Check actual service-cgroup placement, keeper migration, normal close and
  crash cleanup on an expendable NEW desktop. Preserve existing physical and
  retained desktops. No broad user/session termination is part of acceptance.

Current evidence: the component stages successfully with the development
configuration; this is not privileged installation or runtime acceptance.
