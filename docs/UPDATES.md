# Updates and releases

Updates are signed application images. The clock never accepts an unsigned or older release.

## Default: GitHub

- Push a tag (`git tag v0.3.0 && git push origin v0.3.0`). `.github/workflows/release.yml` builds all eight faces, signs the manifests and creates a GitHub release. It also publishes the browser flasher (`https://vandalon.github.io/ultimatrix/`) and a mirror of the update files (`/updates/`) to GitHub Pages.
- Clocks fetch `https://github.com/<repo>/releases/latest/download/<face>-manifest.json` (plus `.sig` and the image). The repo is `kUpdateRepo` in `firmware/lib/core/UpdateTrust.h`.
- One-time setup (done): `kUpdateRepo` is `vandalon/ultimatrix`, Pages source is GitHub Actions, and the private key is stored:
  `gh secret set UPDATE_SIGNING_KEY < .tools/update-signing.pem`
- A manual run of the workflow (workflow_dispatch) builds without releasing.

## Private server (own hosting, testing)

Settings, Advanced, "Update server": a base URL such as `http://192.168.7.77/ultiMatrix` (http or https). Empty means GitHub. Layout: `<base>/<face>/manifest.json`, `manifest.sig` and the image.

- Build, sign and upload: `firmware/tools/publish-firmware.sh [face ...]`.
- Self-hosters who build their own images must replace the public key in `UpdateTrust.h` with their own and sign with the matching private key.
- Replay protection is kept per source, so test releases on a private server do not block GitHub releases.
