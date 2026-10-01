#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

app = Path(sys.argv[1])
source = Path(__file__).resolve().parents[1] / "fixtures/original-scene.json"
original = hashlib.sha256(source.read_bytes()).hexdigest()
with tempfile.TemporaryDirectory() as folder:
    root = Path(folder)
    output = root / "copy.json"
    command = [str(app), "--open", str(source), "--save-copy", str(output)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    document = json.loads(output.read_text())
    assert document["schemaVersion"] == 5
    assert (root / document["assets"][0]["path"]).resolve() == source.parent / "original-image.png"
    before = output.read_bytes()
    assert subprocess.run(command, capture_output=True, timeout=20).returncode != 0
    assert output.read_bytes() == before
    invalid = root / "invalid.json"
    invalid.write_text("{bad")
    refused = root / "refused.json"
    assert subprocess.run([str(app), "--open", str(invalid), "--save-copy", str(refused)], capture_output=True, timeout=20).returncode != 0
    assert not refused.exists()
    for option, value in (("--select-layer", "99999"), ("--frame", "-1"), ("--frame", "nan"), ("--graph", "bad")):
        assert subprocess.run([str(app), "--open", str(source), option, value], capture_output=True, timeout=20).returncode != 0
assert hashlib.sha256(source.read_bytes()).hexdigest() == original
print("CLI migration, relative media paths, overwrite refusal and invalid-input preservation passed")
