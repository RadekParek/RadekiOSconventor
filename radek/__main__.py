import argparse
import json
import sys
from pathlib import Path
from .pipeline import Pipeline
from .apk import Toolchain, validate_apk, validate_experimental_shell


def main():
    parser = argparse.ArgumentParser(
        description="Authorized IPA inspection and fail-closed static-recompilation assessment"
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("analyze", "convert"):
        p = sub.add_parser(name)
        p.add_argument("ipa", type=Path)
        p.add_argument("--output", type=Path, required=True, help="new, non-existing job directory")
        p.add_argument(
            "--authorized", action="store_true", help="confirm ownership/permission to convert this IPA"
        )
        p.add_argument(
            "--target-abi",
            choices=("auto", "arm64-v8a", "armeabi-v7a"),
            default="auto",
            help="assessment target ABI; auto prefers ARM64 in a FAT IPA and selects ARMv7 for ARM32-only inputs; no APK is emitted",
        )
    p = sub.add_parser(
        "validate-shell",
        help="validate an honestly labelled experimental shell APK (never a complete-game APK)",
    )
    p.add_argument("apk", type=Path)
    p = sub.add_parser("validate")
    p.add_argument("apk", type=Path)
    p.add_argument("--package", required=True)
    p.add_argument("--entry", required=True)
    p.add_argument(
        "--abi",
        choices=("arm64-v8a", "armeabi-v7a"),
        default=None,
        help="expected native ABI; inferred from conversion provenance when omitted",
    )
    p.add_argument(
        "--converter-app",
        action="store_true",
        help="validate the importer APK rather than a converted program",
    )
    args = parser.parse_args()
    try:
        if args.command == "validate-shell":
            result = validate_experimental_shell(args.apk, Toolchain.discover())
            print(json.dumps(result, indent=2))
            return 0 if result["status"] == "VALID" else 1
        if args.command == "validate":
            result = validate_apk(
                args.apk,
                Toolchain.discover(),
                args.package,
                args.entry,
                not args.converter_app,
                expected_abi=args.abi,
            )
            print(json.dumps(result, indent=2))
            return 0
        result = Pipeline(args.output).run(
            args.ipa,
            args.authorized,
            args.command == "analyze",
            args.target_abi,
        )
        print(
            json.dumps(
                {
                    "state": result["state"],
                    "report": str(args.output / "report.json"),
                    "blockers": result.get("blockers", []),
                    "error": result.get("error"),
                },
                indent=2,
            )
        )
        return {"READY": 0, "PARTIAL": 2, "BLOCKED": 3, "FAILED": 1}[result["state"]]
    except Exception as exc:
        print(f"{type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
