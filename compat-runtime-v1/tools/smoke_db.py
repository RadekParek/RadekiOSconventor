#!/usr/bin/env python3
"""Validate or record evidence-backed per-app compat-runtime-v1 smoke results."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
import re
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DATABASE = ROOT / "compatibility" / "database.json"
CONTRACT = "compat-runtime-v1"
FIXED_STATUSES = {"not runnable", "menu", "playable"}
CRASH_STATUS = re.compile(r"^crashes at (\S+)$")


def load_database(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read compatibility database: {error}") from error
    validate_database(data)
    return data


def validate_status(status: str) -> None:
    if status not in FIXED_STATUSES and CRASH_STATUS.fullmatch(status) is None:
        raise ValueError(
            "status must be 'not runnable', 'crashes at <symbol>', 'menu', or 'playable'"
        )


def validate_database(data: Any) -> None:
    if not isinstance(data, dict) or data.get("runtimeContract") != CONTRACT:
        raise ValueError(f"database must declare runtimeContract={CONTRACT!r}")
    if data.get("schemaVersion") != 1:
        raise ValueError("unsupported compatibility database schemaVersion")
    apps = data.get("apps")
    backlog = data.get("shimBacklog")
    if not isinstance(apps, list) or not isinstance(backlog, list):
        raise ValueError("database apps and shimBacklog must be arrays")

    app_ids: set[str] = set()
    for app in apps:
        if not isinstance(app, dict) or not isinstance(app.get("appId"), str):
            raise ValueError("each app record requires a stable appId")
        if app["appId"] in app_ids:
            raise ValueError(f"duplicate appId: {app['appId']}")
        app_ids.add(app["appId"])
        latest = app.get("latestSmoke")
        runs = app.get("smokeRuns")
        if not isinstance(latest, dict) or not isinstance(runs, list) or not runs:
            raise ValueError(f"app {app['appId']} requires a latestSmoke and smokeRuns history")
        validate_status(latest.get("status", ""))
        if not isinstance(latest.get("evidence"), str) or not latest["evidence"].strip():
            raise ValueError(f"app {app['appId']} latestSmoke must cite evidence")

    ranks: set[int] = set()
    family_ids: set[str] = set()
    for family in backlog:
        if not isinstance(family, dict):
            raise ValueError("shimBacklog entries must be objects")
        rank = family.get("rank")
        family_id = family.get("familyId")
        if not isinstance(rank, int) or rank < 1 or rank in ranks:
            raise ValueError("shim family ranks must be unique positive integers")
        if not isinstance(family_id, str) or not family_id or family_id in family_ids:
            raise ValueError("shim family IDs must be non-empty and unique")
        ranks.add(rank)
        family_ids.add(family_id)
        unblocked = family.get("gamesUnblocked")
        if not isinstance(unblocked, list) or any(app_id not in app_ids for app_id in unblocked):
            raise ValueError(f"family {family_id} has an invalid gamesUnblocked list")
    if ranks and ranks != set(range(1, len(ranks) + 1)):
        raise ValueError("shim family ranks must be contiguous starting at 1")


def symbol_names(report: dict[str, Any], key: str) -> list[str]:
    values = report.get(key, [])
    if not isinstance(values, list):
        raise ValueError(f"runtime report field {key} must be an array")
    result: list[str] = []
    for value in values:
        if not isinstance(value, dict) or not isinstance(value.get("symbol"), str):
            raise ValueError(f"runtime report {key} entries must include a symbol name")
        result.append(value["symbol"])
    return result


def read_runtime_report(path: Path | None) -> dict[str, Any] | None:
    if path is None:
        return None
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"could not read runtime report: {error}") from error
    if report.get("runtimeContract") != CONTRACT:
        raise ValueError(f"runtime report must declare runtimeContract={CONTRACT!r}")
    return report


def record_smoke(args: argparse.Namespace) -> None:
    database_path = Path(args.database)
    database = load_database(database_path)
    report = read_runtime_report(Path(args.report) if args.report else None)
    validate_status(args.status)
    if not args.evidence.strip():
        raise ValueError("--evidence is required; smoke statuses must be auditable")

    family_ids = {item["familyId"] for item in database["shimBacklog"]}
    resolved_families = sorted(set(args.family_resolved or []))
    unknown = set(resolved_families) - family_ids
    if unknown:
        raise ValueError(f"unknown --family-resolved value(s): {', '.join(sorted(unknown))}")
    if resolved_families and args.status not in {"menu", "playable"}:
        raise ValueError("shim families can only be marked game-unblocking at menu or playable status")

    first_missing = report.get("firstMissingImport") if report else args.first_missing_import
    if report and args.first_missing_import and args.first_missing_import != first_missing:
        raise ValueError("--first-missing-import conflicts with the supplied runtime report")
    if first_missing is not None and not isinstance(first_missing, str):
        raise ValueError("firstMissingImport must be a symbol string or null")
    resolved_symbols = symbol_names(report, "resolvedSymbols") if report else []
    unresolved_symbols = symbol_names(report, "unresolvedSymbols") if report else []
    crash_match = CRASH_STATUS.fullmatch(args.status)
    crash_symbol = crash_match.group(1) if crash_match else None

    app = next((entry for entry in database["apps"] if entry["appId"] == args.app_id), None)
    if app is None:
        app = {"appId": args.app_id, "name": args.name, "version": args.version,
               "smokeRuns": []}
        database["apps"].append(app)
    else:
        app["name"] = args.name or app.get("name", args.app_id)
        app["version"] = args.version or app.get("version", "unknown")
    now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")
    run = {
        "testedAt": now,
        "status": args.status,
        "device": args.device or "not recorded",
        "runtimeBuild": args.runtime_build or "not recorded",
        "evidence": args.evidence,
        "reportArtifact": "compat-runtime-v1-report.json" if report else None,
        "firstMissingImport": first_missing,
        "crashSymbol": crash_symbol,
        "resolvedSymbols": resolved_symbols,
        "unresolvedSymbols": unresolved_symbols,
        "familiesResolved": resolved_families,
    }
    app["latestSmoke"] = run
    app["smokeRuns"].append(run)

    for family in database["shimBacklog"]:
        unblocked = set(family.get("gamesUnblocked", []))
        if family["familyId"] in resolved_families and args.app_id not in unblocked:
            unblocked.add(args.app_id)
        family["gamesUnblocked"] = sorted(unblocked)
        if family["familyId"] in resolved_families:
            family["gamesUnblockedMeasured"] = True

    validate_database(database)
    database_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = database_path.with_suffix(database_path.suffix + ".tmp")
    temporary.write_text(json.dumps(database, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(database_path)
    print(f"recorded {args.app_id}: {args.status}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    validate = subparsers.add_parser("validate", help="validate the smoke database")
    validate.add_argument("--database", default=str(DEFAULT_DATABASE))
    record = subparsers.add_parser("record", help="append an evidence-backed smoke run")
    record.add_argument("--database", default=str(DEFAULT_DATABASE))
    record.add_argument("--app-id", required=True)
    record.add_argument("--name", required=True)
    record.add_argument("--version", default="unknown")
    record.add_argument("--status", required=True)
    record.add_argument("--evidence", required=True)
    record.add_argument("--device")
    record.add_argument("--runtime-build")
    record.add_argument("--report", help="compat-runtime-v1-report.json from the same smoke run")
    record.add_argument("--first-missing-import")
    record.add_argument("--family-resolved", action="append", default=[])
    args = parser.parse_args(argv)
    try:
        if args.command == "validate":
            data = load_database(Path(args.database))
            print(f"valid {CONTRACT} database: {len(data['apps'])} app(s), "
                  f"{len(data['shimBacklog'])} ranked shim family/families")
        else:
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", args.app_id):
                raise ValueError("--app-id must be a stable 1-128 character identifier")
            record_smoke(args)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
