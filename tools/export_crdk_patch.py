#!/usr/bin/env python3
"""Export edits in a private CrDK clone as kShimC's CrDK patch.

The clone must be based on the revision pinned in ports/crdk/UPSTREAM.json.
Its edits may be committed on top of that revision, staged or left in the
work tree; new files are included. The manifest's patch hash is refreshed.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
work = Path(sys.argv[1]).resolve()
port = root / "ports/crdk"
manifest = json.loads((port / "UPSTREAM.json").read_text())
base = manifest["revision"]
subprocess.run(["git", "merge-base", "--is-ancestor", base, "HEAD"], cwd=work, check=True)
subprocess.run(["git", "add", "-N", "."], cwd=work, check=True)
patch = subprocess.check_output(["git", "diff", "--binary", base], cwd=work)
name = "0001-freestanding-resources.patch"
(port / "patches" / name).write_bytes(patch)
manifest["patches"] = [{"file": name, "sha256": hashlib.sha256(patch).hexdigest()}]
(port / "UPSTREAM.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"Exported {len(patch)} bytes: {name}")
