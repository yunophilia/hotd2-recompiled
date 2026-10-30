"""Dev server for the browser build.

Sends the COOP/COEP headers that SharedArrayBuffer (pthreads) needs and
correct MIME types (Python's default map serves .js as text/plain on Windows).
`/local-roms/<name>` serves files from ROM_DIR so you can test with your own
dump without picking files each time. That route exists only on this local server.

    python web/serve.py [port] [--public]

--public is for serving through a tunnel (docs/SELF_HOSTING.md): it turns off the
debug routes (/local-roms/ would hand your ROM to every visitor, /replay/ serves
memory dumps) and lets browsers and Cloudflare cache the game build.
"""
import http.server
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
ROM_DIR = os.environ.get("HOTD2_ROM_DIR", r"C:\RetroBat\roms\naomi")
ROM_FILES = {"hotd2.zip", "hod2bios.zip"}
PUBLIC = "--public" in sys.argv


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".js": "text/javascript",
        ".mjs": "text/javascript",
        ".wasm": "application/wasm",
        ".html": "text/html",
        ".css": "text/css",
        ".json": "application/json",
    }

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=ROOT, **kw)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        if PUBLIC and self.path.startswith("/game/"):
            # the build changes only when you rebuild; a day is a safe cache lifetime
            self.send_header("Cache-Control", "public, max-age=86400")
        else:
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        if PUBLIC and (path.startswith("/local-roms/") or path.startswith("/replay/")):
            return os.path.join(ROOT, "__missing__")
        if path.startswith("/local-roms/"):
            name = path.split("/")[-1].split("?")[0]
            if name in ROM_FILES:
                return os.path.join(ROM_DIR, name)
            return os.path.join(ROOT, "__missing__")
        return super().translate_path(path)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if args else 8642
    print(f"serving {ROOT} on http://localhost:{port}" + (" (public mode)" if PUBLIC else ""))
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
