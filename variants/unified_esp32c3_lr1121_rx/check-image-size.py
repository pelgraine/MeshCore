"""Check the padded ESP image, rather than just the ELF's flash sections."""

import csv
from pathlib import Path

Import("env")


def check_image_size(source, target, env):
    partitions = Path(env.subst("$PROJECT_DIR")) / env.GetProjectOption("board_build.partitions")
    with partitions.open() as stream:
        rows = csv.reader(line for line in stream if not line.lstrip().startswith("#"))
        app_sizes = [int(row[4].strip(), 0) for row in rows
                     if len(row) >= 5 and row[1].strip() == "app"]
    maximum = min(app_sizes)
    image = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
    size = image.stat().st_size
    if size > maximum:
        raise RuntimeError(
            f"{image.name}: actual image ({size} bytes) exceeds application "
            f"slot ({maximum} bytes), including image padding"
        )
    print(f"Actual image: {size} / {maximum} bytes ({maximum - size} bytes free per slot)")


# An always-built dependency also checks an already up-to-date firmware.bin.
check = env.AddCustomTarget(
    "check_image_size", "$BUILD_DIR/${PROGNAME}.bin", check_image_size,
    title="Check actual application image size", always_build=True,
)
env.Default(check)
env.Depends("mergebin", check)
env.Depends("upload", check)
