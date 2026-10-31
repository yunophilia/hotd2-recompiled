# Self-hosting with a Cloudflare Tunnel

Play your build from any device, behind a login, served from your own PC.
Nothing is public: Cloudflare Access only lets in the email addresses you list.
Keep that list to people who own the game; the build in `web/game/` is derived
from Sega's code and must not be shared openly.

Cost: about $10/year for a domain. The tunnel and Access (up to 50 users) are free.

## 0. What you need

- A domain using Cloudflare DNS: buy one in the Cloudflare dashboard
  (Domain Registration), or add an existing domain and switch its nameservers
  to Cloudflare. Below it is `example.com`; the game will live at
  `hotd2.example.com`.
- A built game (`runtime/build-web.sh`, so `web/game/hotd2.js` and `.wasm` exist).
- Windows with `winget` (macOS/Linux: install `cloudflared` from Cloudflare's
  package instructions; the commands are the same).

## 1. Start the server in public mode

```powershell
python web/serve.py 8642 --public
```

`--public` turns off the debug routes (`/local-roms/` would otherwise hand your
ROM to every visitor, `/replay/` serves memory dumps) and lets browsers and
Cloudflare cache the game build. Players load their own `hotd2.zip` with the
file picker, so only the ~15 MB build travels from your PC.

## 2. Install cloudflared and log in

```powershell
winget install --id Cloudflare.cloudflared
cloudflared tunnel login
```

`login` opens a browser: pick `example.com`. It saves a certificate to
`%USERPROFILE%\.cloudflared\cert.pem`.

## 3. Create the tunnel and its DNS name

```powershell
cloudflared tunnel create hotd2
cloudflared tunnel route dns hotd2 hotd2.example.com
```

`create` prints a tunnel ID and writes `%USERPROFILE%\.cloudflared\<TUNNEL-ID>.json`.

## 4. Configure it

Create `%USERPROFILE%\.cloudflared\config.yml`:

```yaml
tunnel: <TUNNEL-ID>
credentials-file: C:\Users\<you>\.cloudflared\<TUNNEL-ID>.json

ingress:
  - hostname: hotd2.example.com
    service: http://localhost:8642
  - service: http_status:404
```

## 5. Lock it down with Cloudflare Access (do this before running the tunnel)

1. Cloudflare dashboard → **Zero Trust** (first time: pick a team name and the
   Free plan).
2. **Access → Applications → Add an application → Self-hosted**.
3. Application domain: `hotd2.example.com`. Session duration: e.g. 1 month.
4. Add a policy: Action **Allow**, Include → **Emails** → your address and your
   friends' addresses.
5. Login method: the default **One-time PIN** (a code is emailed) works without
   any extra setup.

Anyone not on the list gets a Cloudflare login page and never reaches your PC.

## 6. Run the tunnel

```powershell
cloudflared tunnel run hotd2
```

Open `https://hotd2.example.com/play/`, log in with your email, load your
`hotd2.zip`. The tunnel and `serve.py` only need to run while you play; close
both windows to take it offline.

## 7. Optional: cache the build at Cloudflare's edge

The server already marks `/game/*` cacheable for a day, so returning players
load it from their own browser cache. To also let Cloudflare keep a copy (fast
first loads for everyone after the first one, even on a slow home upload):

Dashboard → `example.com` → **Caching → Cache Rules → Create rule**:
- When: Hostname equals `hotd2.example.com` **and** URI Path starts with `/game/`
- Then: **Eligible for cache**, Edge TTL: respect origin (or 1 day)

After you rebuild the game, purge it: **Caching → Configuration → Purge Cache →
Custom purge** → `https://hotd2.example.com/game/hotd2.wasm` and `.../hotd2.js`.

## Notes

- **Headers:** the COOP/COEP headers the threaded build needs come from
  `serve.py` and pass through the tunnel unchanged; no service-worker shim needed.
- **Always on:** to keep it up without a terminal, run `cloudflared` as a Windows
  service (`cloudflared service install`, see Cloudflare's docs for where the
  service expects `config.yml`) and start `serve.py` with Task Scheduler; or run
  both on a small VPS.
- **Troubleshooting:** 502 from Cloudflare means `serve.py` is not running on
  port 8642. A blank page usually means an old cached build: hard-reload
  (Ctrl+Shift+R) or purge the Cloudflare cache.
