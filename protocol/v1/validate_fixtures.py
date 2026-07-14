#!/usr/bin/env python3
"""Validate protocol v1 fixtures and the stable error registry."""

import json
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker


ROOT = Path(__file__).resolve().parent


def load(path: Path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def main() -> None:
    schema = load(ROOT / "schema.json")
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema, format_checker=FormatChecker())

    fixtures = sorted((ROOT / "fixtures").glob("*.json"))
    if not fixtures:
        raise SystemExit("no protocol fixtures found")
    for fixture in fixtures:
        validator.validate(load(fixture))

    invalid_fixtures = sorted((ROOT / "fixtures-invalid").glob("*.json"))
    if not invalid_fixtures:
        raise SystemExit("no invalid protocol fixtures found")
    for fixture in invalid_fixtures:
        errors = list(validator.iter_errors(load(fixture)))
        if not errors:
            raise SystemExit(f"invalid fixture unexpectedly passed: {fixture.name}")

    registry = load(ROOT / "errors.json")
    codes = [entry["code"] for entry in registry["errors"]]
    if len(codes) != len(set(codes)):
        raise SystemExit("duplicate protocol error code")
    if registry["protocol_version"] != 1:
        raise SystemExit("error registry version does not match protocol")

    print(
        f"validated {len(fixtures)} valid fixtures, "
        f"{len(invalid_fixtures)} invalid fixtures, and {len(codes)} error codes"
    )


if __name__ == "__main__":
    main()
