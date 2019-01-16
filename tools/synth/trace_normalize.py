#!/usr/bin/env python3
"""trace_normalize takes a trace file and removes sources of non-determinism.

Some examples of non-determinism are times and object IDs.
The script can also apply transforms to make it easier to read by a human, such
as translating doubles from the encoded format to a human-readable number.

The purpose of this is to be able to compare two traces for similarity, and
focus on the different events that occur rather than trivial differences like
times. You can use diff freely on a trace after it has been normalized.

You can also re-run a trace after it has been normalized through the
TraceInterpreter; you could submit this if you like as a benchmark.
However, it is typically better to submit the original, to keep the times in
case they're ever needed.
"""
import argparse
import json
import struct
import sys
from collections import defaultdict
from typing import Union


def isValueObject(v: str) -> bool:
    return v.startswith("object:")


def isValueNumber(v: str) -> bool:
    return v.startswith("number:")


def parseObjectFromValue(v: str) -> int:
    assert isValueObject(v)
    parts = v.split(":")
    assert len(parts) == 2
    return int(parts[1])


def parseNumberFromValue(v: str) -> Union[int, float]:
    assert isValueNumber(v)
    parts = v.split(":")
    assert len(parts) == 2
    num = int(parts[1], base=16)
    flt = struct.unpack("d", struct.pack("Q", num))[0]
    return int(flt) if flt.is_integer() else flt


def createObject(objID: int) -> str:
    return "object:" + str(objID)


def createNumber(num: float) -> str:
    # This might be an imprecise notation, but it is more human-readable than
    # the precise version.
    return "number:" + str(num)


class Normalizer:
    def __init__(self, globalObjID: int, convert_number: bool = False):
        def get_normalize_map(globalObjID: int):
            def factory():
                id = 0

                def get_next_id():
                    nonlocal id
                    id += 1
                    return id

                return get_next_id

            return defaultdict(factory(), {globalObjID: 0})

        self.normal = get_normalize_map(globalObjID)
        self.convert_number = convert_number

    def normalize_value(self, v: str) -> str:
        if isValueObject(v):
            return createObject(self.normal[parseObjectFromValue(v)])
        elif self.convert_number and isValueNumber(v):
            return createNumber(parseNumberFromValue(v))
        else:
            return v

    def normalize_rec(self, rec):
        for objkey in ["objID", "functionID", "hostObjectID"]:
            if objkey in rec:
                rec[objkey] = self.normal[rec[objkey]]

        for valuekey in ["value", "retval"]:
            if valuekey in rec:
                rec[valuekey] = self.normalize_value(rec[valuekey])

        if "args" in rec:
            # Args is an array of values, normalize each one if it's an object
            rec["args"] = [self.normalize_value(v) for v in rec["args"]]
        return rec


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "infile", nargs="?", type=argparse.FileType("r"), default=sys.stdin
    )
    parser.add_argument(
        "outfile", nargs="?", type=argparse.FileType("w"), default=sys.stdout
    )
    parser.add_argument(
        "--convert-number", dest="convert_number", action="store_true", default=False
    )
    args = parser.parse_args()
    trace_contents = json.load(args.infile)
    normal = Normalizer(trace_contents["globalObjID"], args.convert_number)

    def stripTime(rec):
        del rec["time"]
        return rec

    trace_contents["trace"] = [
        normal.normalize_rec(stripTime(rec)) for rec in trace_contents["trace"]
    ]
    trace_contents["globalObjID"] = 0
    json.dump(trace_contents, args.outfile, indent=4)
    return 0


if __name__ == "__main__":
    sys.exit(main())
