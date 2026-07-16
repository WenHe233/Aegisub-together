#!/usr/bin/env python3
"""Validate protocol v2 fixtures and the stable error registry."""

import json
from pathlib import Path

from jsonschema import Draft202012Validator

ROOT = Path(__file__).resolve().parent

def load(path: Path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)

def main() -> None:
    schema = load(ROOT / "schema.json")
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema)
    fixtures = sorted((ROOT / "fixtures").glob("*.json"))
    invalid = sorted((ROOT / "fixtures-invalid").glob("*.json"))
    if not fixtures or not invalid:
        raise SystemExit("protocol v2 fixtures are incomplete")
    for fixture in fixtures:
        validator.validate(load(fixture))
    for fixture in invalid:
        if not list(validator.iter_errors(load(fixture))):
            raise SystemExit(f"invalid fixture unexpectedly passed: {fixture.name}")
    registry = load(ROOT / "errors.json")
    codes = [entry["code"] for entry in registry["errors"]]
    if registry["protocol_version"] != 2 or len(codes) != len(set(codes)):
        raise SystemExit("invalid protocol v2 error registry")
    print(f"validated {len(fixtures)} valid fixtures, {len(invalid)} invalid fixtures, and {len(codes)} error codes")

if __name__ == "__main__":
    main()
