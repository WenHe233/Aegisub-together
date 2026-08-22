#!/usr/bin/env python3

import pathlib
import sys
import zipfile


source = pathlib.Path(sys.argv[1])
manifest = pathlib.Path(sys.argv[2])
output = pathlib.Path(sys.argv[3])

names = [
    line.strip()
    for line in manifest.read_text(encoding="utf-8").splitlines()
    if line.strip()
]

output.parent.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for name in names:
        path = source / pathlib.PurePosixPath(name)
        if not path.is_file():
            raise FileNotFoundError(path)
        info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o100644 << 16
        archive.writestr(info, path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
