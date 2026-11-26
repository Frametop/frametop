"""Developer CLI; model inspection and compilation need no headset."""

import argparse
import json
from pathlib import Path

from . import Device, __version__
from .compiler import build_job
from .errors import FrameNPUError
from .model import Model
from .ncnn import inspect_ncnn
from .resident import load_resident_plan


def main(argv=None):
    parser = argparse.ArgumentParser(prog="framenpu")
    parser.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = parser.add_subparsers(dest="command", required=True)
    audit = sub.add_parser("inspect-ncnn", help="inventory an ncnn graph without running it")
    audit.add_argument("param", type=Path)
    sub.add_parser("info", help="read CDSP ABI and current operation/callback status")
    inspect = sub.add_parser("inspect-model", help="validate a model and show its execution plan")
    inspect.add_argument("model", type=Path)
    inspect.add_argument("--batch-size", type=int, default=1)
    resident = sub.add_parser(
        "plan-resident", help="plan resident DDR storage offline; no execution"
    )
    resident.add_argument("schedule", type=Path, help="version 1 resident storage schedule JSON")
    run = sub.add_parser("run-model", help="run a supported model on the headset")
    run.add_argument("model", type=Path)
    run.add_argument("input", type=Path, help="JSON vector or batch")
    build = sub.add_parser("build-job", help="compile a custom C kernel using Hexagon LLVM")
    build.add_argument("source", type=Path)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--hmx", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.command == "inspect-ncnn":
            print(json.dumps(inspect_ncnn(args.param), indent=2))
        elif args.command == "info":
            with Device() as device:
                print(json.dumps(device.status(), indent=2))
        elif args.command == "inspect-model":
            model = Model.load(args.model)
            print(json.dumps({"name": model.name, "plan": model.plan(args.batch_size)}, indent=2))
        elif args.command == "plan-resident":
            print(json.dumps(load_resident_plan(args.schedule), indent=2))
        elif args.command == "run-model":
            model = Model.load(args.model)
            inputs = json.loads(args.input.read_text())
            with Device() as device:
                print(json.dumps(model.run(device, inputs)))
        else:
            job = build_job(args.source, args.output, requires_hmx=args.hmx)
            print(
                json.dumps({"name": job.name, "bytes": len(job.code), "output": str(args.output)})
            )
    except (FrameNPUError, OSError, ValueError) as exc:
        parser.exit(2, f"framenpu: {exc}\n")


if __name__ == "__main__":
    main()
