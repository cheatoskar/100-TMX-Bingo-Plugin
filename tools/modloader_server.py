#!/usr/bin/env python3
"""ModLoader (TMLoader) update server for 100% TMX + Bingo.

A ModLoader server is a folder URL whose index.json lists products and their versions
(format of https://fullspeed.mooo.com/modloader/index.json, api 1). For every version the
ModLoader downloads `download_link` (a zip), checks its SHA-256 against `hash_sha256`, unpacks it
into database/TmForever/products/<product>/<version>/ and writes the version entry next to it
as description.yaml. So the zip holds only the version folder's files (here: the DLL), flat.

The product name and description come from install-modloader.ps1, the script that writes the
same product folder by hand and in CI, so the server and the zip can't disagree about them.
Only icon_link differs: the index points at the copy served next to it on GitHub Pages.

  zip       100TMX.dll -> 100TMX-ModLoader-server.zip (byte-identical for the same DLL)
  index     index.json (stable releases) and beta/index.json (stable + pre-releases) from the
            GitHub releases' 100TMX.dll (from MIN_VERSION on). Each version's zip is written next
            to the index (files/<version>/100TMX-ModLoader-server.zip) and served from there, so
            the releases carry no ModLoader zip that people download by mistake; changelogs from
            the release notes (RELEASE_NOTES.md as it was at that tag)
  check     validate index.json files and zips against the format and TMLoader's own field list
  selftest  zip + index + check from a local DLL and fake releases (no network needed)

Standard library only (runs on the GitHub runners and with `python -I`).
"""
import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.request
import zipfile

REPO = "cheatoskar/100-TMX-Bingo-Plugin"
PRODUCT = "100% TMX + Bingo"  # the product folder name; install-modloader.ps1 uses the same folder
ASSET = "100TMX-ModLoader-server.zip"  # not 100TMX-ModLoader.zip: v0.9.5 carries one with a folder inside
FILES = "files"  # <index folder>/files/<version>/100TMX-ModLoader-server.zip
DLL = "100TMX.dll"
GAME = "TmForever"
ICON_URL = "https://cheatoskar.github.io/100-TMX-Bingo-Plugin/modloader/icon.png"
MIN_VERSION = "1.1.0"  # the first version on the server; older releases are not listed
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INSTALLER = os.path.join(ROOT, "install-modloader.ps1")
CHANGELOG_LIMIT = 6000  # characters per version; the ModLoader shows it as plain text
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")


# ---------------------------------------------------------------------------------------------
# metadata: the YAML here-strings in install-modloader.ps1 (flat files; no YAML library needed)

def _yaml_scalar(text):
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] == "'":
        return text[1:-1].replace("''", "'")
    if len(text) >= 2 and text[0] == text[-1] == '"':
        return json.loads(text)
    return text


def _here_string(ps1_text, target):
    """The body of `WriteAllText("<target>\\description.yaml", @" ... "@`."""
    m = re.search(r'WriteAllText\("\$' + target + r'\\description\.yaml",\s*@"\r?\n(.*?)\r?\n"@', ps1_text, re.S)
    if not m:
        raise SystemExit(f"install-modloader.ps1: no description.yaml written to ${target}")
    return m.group(1)


def read_metadata(path=INSTALLER):
    """-> (product meta, version meta) from install-modloader.ps1."""
    text = open(path, encoding="utf-8").read()
    product = {}
    for line in _here_string(text, "product").splitlines():
        m = re.match(r"^([a-z_]+):\s*(.*)$", line.rstrip())
        if m and m.group(2):
            product[m.group(1)] = _yaml_scalar(m.group(2))
    if product.get("name") != PRODUCT:
        raise SystemExit(f"install-modloader.ps1: product name {product.get('name')!r} != {PRODUCT!r}")
    product["icon_link"] = ICON_URL

    vtext = _here_string(text, "target")
    executable = re.search(r"^executable:\s*(.+)$", vtext, re.M)
    if not executable:
        raise SystemExit("install-modloader.ps1: no executable")
    deps = []
    for m in re.finditer(r"^\s*-\s*id:\s*(.+)$((?:\r?\n\s+version:\s*.+$)?)", vtext, re.M):
        dep = {"id": _yaml_scalar(m.group(1))}
        v = re.search(r"version:\s*(.+)$", m.group(2), re.M)
        if v:
            dep["version"] = _yaml_scalar(v.group(1))
        deps.append(dep)
    return product, {"executable": _yaml_scalar(executable.group(1)), "dependencies": deps}


# ---------------------------------------------------------------------------------------------
# zip

def make_zip(dll, out):
    """The DLL alone at the zip's root, fixed timestamp: the same DLL gives the same bytes."""
    info = zipfile.ZipInfo(DLL, date_time=(2020, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    info.create_system = 0
    with open(dll, "rb") as f:
        data = f.read()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w") as z:
        z.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    return sha256_file(out)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------------------------
# changelog

def semver_key(version):
    m = SEMVER.match(version)
    if not m:
        return (0, 0, 0, 0, ())
    pre = m.group(4)
    ids = ()
    if pre:
        ids = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split("."))
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if pre else 1, ids)


def release_changes(body):
    """The part of a release body that says what changed, for people who don't download by hand.

    The body is RELEASE_NOTES.md (a '## 100% TMX + Bingo x.y.z' title, a paragraph or a
    '### Download' section on installing by hand, '### ...' sections), then the hash table
    ('### SHA-256') and GitHub's generated '## What's Changed'. Kept: the '###' sections other
    than download/install ones; the text before the first of them only if it isn't about the
    download."""
    body = re.split(r"^(?:###\s+SHA-256|##\s+What's Changed|<details>)", body or "", maxsplit=1, flags=re.M)[0]
    lines = [l for l in body.splitlines() if not re.match(r"^##\s+(?!#)", l)]
    parts = [("", [])]
    for line in lines:
        m = re.match(r"^###\s+(.*)$", line)
        if m:
            parts.append((m.group(1).strip(), [line]))
        else:
            parts[-1][1].append(line)
    kept = []
    for i, (heading, part) in enumerate(parts):
        text = "\n".join(part).strip()
        if not text:
            continue
        if i == 0 and len(parts) > 1 and re.search(r"100TMX\.zip|Installer\.exe", text):
            continue
        if re.match(r"(?i)^(download|install)", heading):
            continue
        kept.append(text)
    return "\n\n".join(kept)


_TYPO = {"→": "->", "←": "<-", "–": "-", "—": "-", "‘": "'", "’": "'",
         "“": '"', "”": '"', "…": "...", " ": " ", "−": "-", "×": "x"}


def markdown_to_text(md, limit=CHANGELOG_LIMIT):
    """Markdown -> the plain text the ModLoader shows (white-space: pre-wrap, no markup)."""
    out = []
    for line in md.splitlines():
        s = line.rstrip()
        if re.match(r"^\s*!\[.*\]\(.*\)\s*$", s) or re.match(r"^\s*(---+|\*\*\*+)\s*$", s):
            continue
        if s.strip().lower().startswith(("<details", "</details", "<summary")):
            continue
        s = re.sub(r"!\[[^\]]*\]\([^)]*\)", "", s)
        s = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", s)
        heading = re.match(r"^#{1,6}\s+(.*)$", s)
        if heading:
            s = heading.group(1).rstrip(":") + ":"
        bold_line = re.match(r"^\*\*(.+?)\*\*:?$", s.strip())
        if bold_line:
            s = bold_line.group(1).rstrip(":") + ":"
        s = s.replace("**", "").replace("__", "").replace("`", "")
        s = re.sub(r"(?<![\w*])\*(?!\s)([^*\n]+?)(?<!\s)\*(?![\w*])", r"\1", s)
        s = re.sub(r"^(\s*)\* ", r"\1- ", s)
        for k, v in _TYPO.items():
            s = s.replace(k, v)
        out.append(s)
    text = re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip()
    if len(text) > limit:
        cut = text.rfind("\n", 0, limit)
        text = text[: cut if cut > 0 else limit].rstrip() + "\n..."
    return text


def changelog_for(version, release):
    text = markdown_to_text(release_changes(release.get("body")))
    url = release.get("html_url") or f"https://github.com/{REPO}/releases/tag/v{version}"
    return (text + "\n\n" if text else "") + "Release notes: " + url


# ---------------------------------------------------------------------------------------------
# GitHub releases

def _get(url, token=None, accept="application/vnd.github+json"):
    req = urllib.request.Request(url, headers={"User-Agent": "100tmx-modloader-index", "Accept": accept})
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    return urllib.request.urlopen(req, timeout=120)


def fetch_releases(repo, token=None):
    releases, url = [], f"https://api.github.com/repos/{repo}/releases?per_page=100"
    while url:
        with _get(url, token) as r:
            releases += json.load(r)
            m = re.search(r'<([^>]+)>;\s*rel="next"', r.headers.get("Link") or "")
            url = m.group(1) if m else None
    return releases


def load_releases(args):
    if args.releases:
        with open(args.releases, encoding="utf-8") as f:
            return json.load(f)
    return fetch_releases(args.repo, os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN"))


def fetch_asset(asset, cache_dir, local):
    """The release asset as a local file: downloaded once into the cache, checked against the
    digest the API lists."""
    name = asset["browser_download_url"]
    if name in local:
        return local[name]
    os.makedirs(cache_dir, exist_ok=True)
    path = os.path.join(cache_dir, hashlib.sha1(name.encode()).hexdigest() + ".bin")
    if not os.path.exists(path):
        with _get(name, accept="application/octet-stream") as r, open(path + ".part", "wb") as f:
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                f.write(chunk)
        os.replace(path + ".part", path)
    digest = sha256_file(path)
    # The API also lists a digest for newer assets: both must agree.
    listed = (asset.get("digest") or "").removeprefix("sha256:")
    if listed and listed != digest:
        raise SystemExit(f"{name}: downloaded sha256 {digest} != listed {listed}")
    return path


def default_base_url(repo):
    """Where GitHub Pages serves the index folder: https://<owner>.github.io/<repo>/modloader."""
    owner, name = repo.split("/", 1)
    return f"https://{owner.lower()}.github.io/{name}/modloader"


def build_index(releases, product_meta, version_meta, zip_for):
    """-> (stable index, beta index, skipped tags)."""
    entries, skipped = [], []
    for rel in releases:
        tag = rel.get("tag_name", "")
        if rel.get("draft") or not tag.startswith("v") or not SEMVER.match(tag[1:]):
            continue
        version = tag[1:]
        dll = next((a for a in rel.get("assets", []) if a.get("name") == DLL), None)
        if not dll or semver_key(version) < semver_key(MIN_VERSION):
            skipped.append(tag)
            continue
        link, digest = zip_for(version, dll)
        entry = {
            "executable": version_meta["executable"],
            "dependencies": version_meta["dependencies"],
            "download_link": link,
            "hash_sha256": digest,
            "changelog": changelog_for(version, rel),
        }
        entries.append((version, bool(rel.get("prerelease")), entry))
    entries.sort(key=lambda e: semver_key(e[0]))

    def index(include_pre):
        product = {k: product_meta[k] for k in ("name", "author", "type", "description", "icon_link", "homepage")
                   if product_meta.get(k)}
        product["versions"] = {v: e for v, pre, e in entries if include_pre or not pre}
        return {"api": 1, "game": GAME, "products": {PRODUCT: product}}

    return index(False), index(True), skipped


def write_json(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=True)
        f.write("\n")


# ---------------------------------------------------------------------------------------------
# check

def tmloader_fields(loader_d_ts):
    """Field names of ProductDescription / ProductVersionDescription in TMLoader's ui/layouts/loader.d.ts."""
    text = open(loader_d_ts, encoding="utf-8").read()
    fields = {}
    for cls in ("ProductDescription", "ProductVersionDescription", "Selector"):
        m = re.search(r"declare class " + cls + r"\b[^{]*\{(.*?)\n\}", text, re.S)
        if m:
            fields[cls] = set(re.findall(r"^\s*(?:readonly\s+)?([a-z_0-9]+)\s*:", m.group(1), re.M)) - {
                "product", "productversion"}
    return fields


def check_index(path, reference=None, fields=None, zips=None, installed=None):
    errors, notes = [], []
    data = json.load(open(path, encoding="utf-8"))
    def err(msg):
        errors.append(f"{path}: {msg}")
    if set(data) != {"api", "game", "products"}:
        err(f"top-level keys {sorted(data)}")
    if data.get("api") != 1:
        err("api is not 1")
    if data.get("game") != GAME:
        err(f"game is not {GAME}")
    products = data.get("products") or {}
    if not isinstance(products, dict) or not products:
        err("no products")
    if PRODUCT not in products:
        err(f"no product {PRODUCT!r} (the folder name the installer uses)")
    ref_product_keys = ref_version_keys = None
    if reference:
        rp = next(iter(reference["products"].values()))
        ref_product_keys = set(rp)
        ref_version_keys = set(next(iter(rp["versions"].values())))
    for pid, prod in products.items():
        keys = set(prod)
        for k in ("name", "author", "type", "versions"):
            if k not in prod:
                err(f"{pid}: missing {k}")
        if prod.get("type") not in ("modification", "program"):
            err(f"{pid}: type {prod.get('type')!r}")
        if fields and "ProductDescription" in fields and keys - {"versions"} - fields["ProductDescription"]:
            err(f"{pid}: fields unknown to TMLoader {sorted(keys - {'versions'} - fields['ProductDescription'])}")
        if ref_product_keys is not None and keys != ref_product_keys:
            notes.append(f"{pid}: product keys differ from reference: ours-ref {sorted(keys - ref_product_keys)}, "
                         f"ref-ours {sorted(ref_product_keys - keys)}")
        for link in ("icon_link", "homepage"):
            if link in prod and not str(prod[link]).startswith("https://"):
                err(f"{pid}: {link} is not https")
        versions = prod.get("versions") or {}
        if not versions:
            notes.append(f"{pid}: no versions")
        for ver, e in versions.items():
            where = f"{pid} {ver}"
            if not SEMVER.match(ver):
                err(f"{where}: version is not semver")
            vkeys = set(e)
            for k in ("executable", "dependencies", "download_link", "hash_sha256"):
                if k not in e:
                    err(f"{where}: missing {k}")
            if fields and "ProductVersionDescription" in fields and vkeys - fields["ProductVersionDescription"]:
                err(f"{where}: fields unknown to TMLoader {sorted(vkeys - fields['ProductVersionDescription'])}")
            if ref_version_keys is not None and vkeys != ref_version_keys:
                err(f"{where}: version keys {sorted(vkeys)} != reference {sorted(ref_version_keys)}")
            if not SHA256.match(str(e.get("hash_sha256", ""))):
                err(f"{where}: hash_sha256 is not 64 lowercase hex digits")
            if not str(e.get("download_link", "")).startswith("https://"):
                err(f"{where}: download_link is not https")
            deps = e.get("dependencies")
            if not isinstance(deps, list) or not all(isinstance(d, dict) and isinstance(d.get("id"), str) for d in deps):
                err(f"{where}: dependencies must be a list of {{id, version}}")
            else:
                for d in deps:
                    extra = set(d) - (fields.get("Selector") if fields and "Selector" in fields else {"id", "version"})
                    if extra:
                        err(f"{where}: dependency fields {sorted(extra)}")
            if not isinstance(e.get("changelog", ""), str):
                err(f"{where}: changelog is not a string")
            if zips and e.get("download_link") in zips:
                zp = zips[e["download_link"]]
                if sha256_file(zp) != e.get("hash_sha256"):
                    err(f"{where}: zip hash differs from hash_sha256")
                with zipfile.ZipFile(zp) as z:
                    names = z.namelist()
                    if z.testzip() is not None:
                        err(f"{where}: zip is corrupt")
                if e.get("executable") not in names:
                    err(f"{where}: {e.get('executable')} is not at the zip's root ({names})")
                if any("/" in n or "\\" in n for n in names):
                    err(f"{where}: the zip has folders; the ModLoader unpacks it into the version folder")
                if "description.yaml" in names:
                    err(f"{where}: description.yaml in the zip (the ModLoader writes it from the index)")
                if installed:
                    want = sorted(set(installed) - {"description.yaml"})
                    notes.append(f"{where}: zip {sorted(names)}; installed reference version folder {want}")
    return errors, notes


def installed_layout(product_dir):
    """Files of the newest version folder of an installed ModLoader product (read only)."""
    if not product_dir or not os.path.isdir(product_dir):
        return None
    versions = [d for d in os.listdir(product_dir) if os.path.isdir(os.path.join(product_dir, d)) and SEMVER.match(d)]
    if not versions:
        return None
    newest = max(versions, key=semver_key)
    return [f for f in os.listdir(os.path.join(product_dir, newest)) if "." in f and not re.search(r"\.dll\.", f)]


def default_loader_d_ts():
    base = os.environ.get("LOCALAPPDATA")
    path = os.path.join(base, "TMLoader", "ui", "layouts", "loader.d.ts") if base else ""
    return path if path and os.path.exists(path) else None


def load_reference(arg):
    if not arg:
        return None
    if re.match(r"^https?://", arg):
        with _get(arg, accept="application/json") as r:
            return json.load(r)
    return json.load(open(arg, encoding="utf-8"))


# ---------------------------------------------------------------------------------------------
# commands

def cmd_zip(args):
    digest = make_zip(args.dll, args.out)
    print(f"{digest}  {os.path.basename(args.out)}")


def cmd_index(args):
    releases = load_releases(args)
    product_meta, version_meta = read_metadata(args.installer)
    local = dict(kv.split("=", 1) for kv in (args.local or []))
    cache = args.cache or os.path.join(tempfile.gettempdir(), "100tmx-modloader-cache")
    base = (args.base_url or default_base_url(args.repo)).rstrip("/")

    def zip_for(version, dll):
        rel_path = f"{FILES}/{version}/{ASSET}"
        digest = make_zip(fetch_asset(dll, cache, local), os.path.join(args.out_dir, *rel_path.split("/")))
        return f"{base}/{rel_path}", digest

    stable, beta, skipped = build_index(releases, product_meta, version_meta, zip_for)
    write_json(os.path.join(args.out_dir, "index.json"), stable)
    write_json(os.path.join(args.out_dir, "beta", "index.json"), beta)
    for name, idx in (("index.json", stable), ("beta/index.json", beta)):
        print(f"{name}: {', '.join(idx['products'][PRODUCT]['versions']) or '(no versions)'}")
    if skipped:
        print(f"not listed (no {DLL}, or older than {MIN_VERSION}): {', '.join(skipped)}")


def cmd_check(args):
    fields = tmloader_fields(args.loader_d_ts) if args.loader_d_ts else None
    reference = load_reference(args.reference)
    zips = dict(kv.split("=", 1) for kv in (args.zip or []))
    installed = installed_layout(args.installed)
    all_errors = []
    for path in args.index:
        errors, notes = check_index(path, reference, fields, zips, installed)
        for n in notes:
            print("note:", n)
        all_errors += errors
    for e in all_errors:
        print("ERROR:", e)
    print("OK" if not all_errors else f"{len(all_errors)} error(s)")
    return 1 if all_errors else 0


# Release bodies shaped like the real ones (RELEASE_NOTES.md + hash table + GitHub's notes).
_BODY_110 = """## 100% TMX + Bingo 1.1.0

Download `100TMX.zip`, extract it and run `100TMX-Installer.exe` (or copy the
folder by hand, see `README.txt`).

### Changes

- New look, based on the **TM Vibrant Shaders** menu.
- The *Take the tile* button now goes away once the tile is yours.

### Updating from 1.0.0

Install as usual.

### SHA-256

| File | SHA-256 |
|---|---|
| `100TMX.zip` | `00` |

## What's Changed
* New menu by @cheatoskar in https://github.com/cheatoskar/100-TMX-Bingo-Plugin/pull/1
"""

_BODY_100 = """## 100% TMX + Bingo 1.0.0 - the first official release

Bingo boards and the [100% TMX](https://100tmx.com) project, inside TrackMania.

### Download

**`100TMX.zip`** is all you need.

### What it does

- **Bingo in the game** - the board.
"""

# The layout from 1.2.0 on: intro, rules, bold-led paragraphs, an Install section, the hashes
# folded away.
_BODY_120 = """## 100% TMX + Bingo 1.2.0

Bingo boards and the [100% TMX](https://100tmx.com) project, inside TrackMania.

---

### Top 10

**The TMX top 10** for the current map.

---

### Install

**With the ModLoader, updates itself:** add this line under `servers:` in `config.yaml`:

```yaml
  - 'https://cheatoskar.github.io/100-TMX-Bingo-Plugin/modloader/'
```

**With the installer:** download `100TMX.zip` and run `100TMX-Installer.exe`.

<details>
<summary>SHA-256 checksums</summary>

| File | SHA-256 |
|---|---|
| `100TMX.zip` | `00` |
</details>

## What's Changed
* Something by @cheatoskar in https://github.com/cheatoskar/100-TMX-Bingo-Plugin/pull/6
"""


def cmd_selftest(args):
    """Fake releases from a local DLL (two stable, one pre-release, one older than MIN_VERSION, one
    without the DLL, one draft), the index and its zips from them, then every check."""
    work = args.work or tempfile.mkdtemp(prefix="100tmx-mlserver-")
    dist = os.path.join(work, "dist")
    os.makedirs(dist, exist_ok=True)
    dll = args.dll
    if not dll:
        for cand in ("build/Release/100TMX.dll", "out/100TMX.dll"):
            if os.path.exists(os.path.join(ROOT, cand)):
                dll = os.path.join(ROOT, cand)
                break
    if not dll:
        dll = os.path.join(work, DLL)
        with open(dll, "wb") as f:
            f.write(b"MZ" + bytes(4094))
        print("no built DLL found: using a dummy file")
    zip_path = os.path.join(dist, ASSET)
    digest = make_zip(dll, zip_path)
    if make_zip(dll, zip_path + ".again") != digest:
        print("ERROR: the zip is not reproducible")
        return 1
    os.remove(zip_path + ".again")
    print(f"zip: {zip_path} ({os.path.getsize(zip_path)} bytes, sha256 {digest})")

    def release(tag, pre, body, with_dll=True):
        assets = [{"name": "SHA256SUMS.txt", "browser_download_url": f"https://github.com/{REPO}/releases/download/{tag}/SHA256SUMS.txt"}]
        if with_dll:
            assets.append({"name": DLL, "browser_download_url": f"https://github.com/{REPO}/releases/download/{tag}/{DLL}"})
        return {"tag_name": tag, "prerelease": pre, "draft": False, "body": body,
                "html_url": f"https://github.com/{REPO}/releases/tag/{tag}", "assets": assets}

    releases = [release("v1.2.0-beta.1", True, "Notes for the beta\n\n<details>\n| x |"),
                release("v1.2.0", False, _BODY_120), release("v1.1.0", False, _BODY_110),
                release("v1.0.0", False, _BODY_100), release("v1.1.1", False, "no DLL", with_dll=False),
                {"tag_name": "v1.3.0", "draft": True, "assets": []}]
    rel_path = os.path.join(work, "releases.json")
    write_json(rel_path, releases)
    local = [f"{a['browser_download_url']}={dll}" for r in releases for a in r["assets"] if a["name"] == DLL]
    site = os.path.join(work, "site", "modloader")
    base = default_base_url(REPO)
    ns = argparse.Namespace(releases=rel_path, repo=REPO, installer=INSTALLER, local=local, cache=None, out_dir=site,
                            base_url=base)
    cmd_index(ns)
    stable = json.load(open(os.path.join(site, "index.json"), encoding="utf-8"))
    beta = json.load(open(os.path.join(site, "beta", "index.json"), encoding="utf-8"))
    problems = []
    prod = stable["products"][PRODUCT]
    if list(prod["versions"]) != ["1.1.0", "1.2.0"]:
        problems.append("stable index must list 1.1.0 and 1.2.0, ascending")
    if list(beta["products"][PRODUCT]["versions"]) != ["1.1.0", "1.2.0-beta.1", "1.2.0"]:
        problems.append("beta index must list 1.1.0, 1.2.0-beta.1 and 1.2.0, ascending")
    for v, e in beta["products"][PRODUCT]["versions"].items():
        if e["download_link"] != f"{base}/{FILES}/{v}/{ASSET}" or e["hash_sha256"] != digest:
            problems.append(f"{v}: the zip must be served next to the index and be the DLL's zip")
    if prod.get("name") != PRODUCT or prod.get("icon_link") != ICON_URL or prod.get("author") != "cheatoskar":
        problems.append("product metadata not taken from install-modloader.ps1 / icon not on Pages")
    if prod["versions"]["1.1.0"]["executable"] != DLL or prod["versions"]["1.1.0"]["dependencies"] != [
            {"id": "CoreMod", "version": "^1.0.1"}]:
        problems.append("executable / dependencies not taken from install-modloader.ps1")
    cl = prod["versions"]["1.1.0"]["changelog"]
    if "**" in cl or "`" in cl or "Changes:" not in cl or "New look" not in cl:
        problems.append("1.1.0 changelog is not the plain-text Changes section")
    if any(s in cl for s in ("100TMX-Installer", "| File |", "What's Changed", "@cheatoskar")):
        problems.append("1.1.0 changelog still has the download paragraph, hash table or GitHub's notes")
    if "Release notes: https://github.com/" not in cl:
        problems.append("changelog has no release link")
    cl120 = prod["versions"]["1.2.0"]["changelog"]
    if "Bingo boards and the 100% TMX project" not in cl120 or "Top 10:" not in cl120:
        problems.append("1.2.0 changelog: keep the intro and the sections")
    if any(s in cl120 for s in ("config.yaml", "100TMX-Installer", "| File |", "---", "<details>")):
        problems.append("1.2.0 changelog still has the install section, the rules or the hash table")
    for p in problems:
        print("ERROR:", p)
    zips = [f"{e['download_link']}={os.path.join(site, FILES, v, ASSET)}"
            for v, e in beta["products"][PRODUCT]["versions"].items()]
    loader = args.loader_d_ts or default_loader_d_ts()
    installed = args.installed
    if installed is None and os.environ.get("LOCALAPPDATA"):
        installed = os.path.join(os.environ["LOCALAPPDATA"], "TMLoader", "database", GAME, "products", PRODUCT)
    print(f"TMLoader field list: {loader or '(not found, skipped)'}; reference: {args.reference or '(none)'}")
    check = argparse.Namespace(index=[os.path.join(site, "index.json"), os.path.join(site, "beta", "index.json")],
                               reference=args.reference, loader_d_ts=loader, zip=zips, installed=installed)
    rc = cmd_check(check)
    print("\n--- index.json (changelog shortened) ---")
    shown = json.loads(json.dumps(stable))
    for e in shown["products"][PRODUCT]["versions"].values():
        e["changelog"] = e["changelog"][:160] + " [...]"
    print(json.dumps(shown, indent=2))
    print(f"\nwork folder: {work}")
    return 1 if (rc or problems) else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("zip")
    p.add_argument("--dll", required=True)
    p.add_argument("--out", required=True)

    def releases_args(p):
        p.add_argument("--releases", help="releases JSON (GitHub API format); default: fetch from the API")
        p.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY") or REPO)

    p = sub.add_parser("index")
    releases_args(p)
    p.add_argument("--out-dir", required=True, help="writes <dir>/index.json and <dir>/beta/index.json")
    p.add_argument("--installer", default=INSTALLER, help="install-modloader.ps1 (product and version metadata)")
    p.add_argument("--local", action="append", help="<DLL download link>=<local DLL>: use this file instead of downloading")
    p.add_argument("--cache", help="download cache folder")
    p.add_argument("--base-url", help="the URL GitHub Pages serves --out-dir at (default: https://<owner>.github.io/<repo>/modloader)")

    p = sub.add_parser("check")
    p.add_argument("index", nargs="+")
    p.add_argument("--reference", help="a working server's index.json (file or URL) to compare the keys with")
    p.add_argument("--loader-d-ts", default=default_loader_d_ts(), help="TMLoader's ui/layouts/loader.d.ts")
    p.add_argument("--zip", action="append", help="<download_link>=<local zip> to check")
    p.add_argument("--installed", help="an installed ModLoader product folder to compare the zip layout with")

    p = sub.add_parser("selftest")
    p.add_argument("--dll", help="default: build/Release/100TMX.dll, else a dummy file")
    p.add_argument("--work", help="work folder (default: a new temp folder)")
    p.add_argument("--reference", help="e.g. https://fullspeed.mooo.com/modloader/index.json")
    p.add_argument("--loader-d-ts")
    p.add_argument("--installed")

    args = ap.parse_args()
    rc = {"zip": cmd_zip, "index": cmd_index, "check": cmd_check, "selftest": cmd_selftest}[args.cmd](args)
    sys.exit(rc or 0)


if __name__ == "__main__":
    main()
