# Updates and releases

Updates are signed application images. The clock never accepts an unsigned release, and refuses an older or already used one unless you force it (below).

## Default: GitHub

- Push a tag (`git tag v0.3.0 && git push origin v0.3.0`). `.github/workflows/release.yml` builds all fourteen faces, signs the manifests and creates a GitHub release. It also publishes the browser flasher (`https://vandalon.github.io/ultimatrix/`) and a mirror of the update files (`/updates/`) to GitHub Pages.
- Stable releases go through a pull request: `dev` -> `main` (release notes as the description, auto-merge once checks pass). After the merge the tag is created on `main` and pushed by hand (a tag pushed by a workflow with `GITHUB_TOKEN` would not start the release workflow), then `dev` is fast-forwarded to `main`. Beta tags are cut on `dev` without a PR.
- Release notes: `firmware/tools/release-notes.py <tag>` builds them from the commit messages (`FEATURE:`/`BUG:`/... lines and their paragraphs) since the previous tag (the previous stable tag for a stable release). A stable tag puts them on its GitHub release; a beta tag creates a file-less pre-release with them. The settings page's update banner links its "What's new" to `https://github.com/vandalon/ultimatrix/releases/tag/<version>`.
- Clocks fetch `https://github.com/<repo>/releases/latest/download/<face>-manifest.json` (plus `.sig` and the image). The repo is `kUpdateRepo` in `firmware/lib/core/UpdateTrust.h`.
- One-time setup (done): `kUpdateRepo` is `vandalon/ultimatrix`, Pages source is GitHub Actions, and the private key is stored:
  `gh secret set UPDATE_SIGNING_KEY < .tools/update-signing.pem`
- Clocks check once a day. With Automatic updates on (default, Settings > Advanced) a newer release is installed by itself; a failed install is retried once after a restart with no face loaded (update screen with progress bar).
- A manual run of the workflow (workflow_dispatch) builds without releasing.

## Beta channel

- Push a tag with `-beta` in it (`git tag v0.8.0-beta.1 && git push origin v0.8.0-beta.1`). The same workflow builds and signs all faces, but publishes no release and leaves Pages alone. It force-pushes the signed files to the single-commit branch `beta` (`<face>/manifest.json`, `manifest.sig`, image), replacing the previous beta.
- Easiest: switch on Settings > Advanced > Firmware updates > Beta updates (it fills in the address below; switching off clears it). Or a clock joins the beta by entering `https://raw.githubusercontent.com/vandalon/ultimatrix/beta` as the update server (Settings > Advanced). Empty goes back to stable releases.
- Replay protection is per source (see below), so beta and stable do not block each other. A clock on stable only moves to a newer stable when it is switched back; the beta source keeps offering the last beta until the next beta tag.

## Forcing a used or superseded release

If an install fails with "This update release has already been used or superseded.", the update banner in Settings shows a **Force update** button. It sends `POST /api/firmware?force=1` (the same `force` that also lets an update pause live sports data), which skips only that release-number check, so it can reinstall the same release or move to an older one. The signed manifest, face, size, SHA-256 checksums and image header are still verified, so an unsigned or altered image is refused. The recorded release number is never lowered, preparing a face ahead of time never forces, the 30-second gap between attempts applies, and with an admin PIN set the PIN is needed. A forced older build still has to pass the boot health check or the clock rolls back.

## Private server (own hosting, testing)

Settings, Advanced, "Update server": a base URL such as `http://192.168.7.77/ultiMatrix` (http or https). Empty means GitHub. Layout: `<base>/<face>/manifest.json`, `manifest.sig` and the image.

- Build, sign and upload: `firmware/tools/publish-firmware.sh [face ...]`.
- Self-hosters who build their own images must replace the public key in `UpdateTrust.h` with their own and sign with the matching private key.
- Replay protection is kept per source, so test releases on a private server do not block GitHub releases.

## Memory

A face's static RAM (`.data` + `.bss`) is reserved in every boot, including the update-only boot that never loads the face, and it comes straight out of the heap the download needs. Every build therefore runs `firmware/tools/check-static-ram.py` and fails above 60 KB (`ULTIMATRIX_STATIC_RAM_KB`; 62 KB for the Home face, 70 KB for Pac-Man with Bluetooth). Large objects (feed tickers, snapshots, caches) are allocated on the heap when the face starts, as the Football and F1/MotoGP faces do. The update-only boot also skips telnet and mDNS.

