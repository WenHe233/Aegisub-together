"""Create the small Windows ZIP consumed by the in-program updater."""

import argparse
import os
import sys
import zipfile


HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))


def package_update(build_dir, output):
    build_dir = os.path.abspath(build_dir)
    output = os.path.abspath(output)
    executable = os.path.join(build_dir, 'aegisub.exe')
    if not os.path.isfile(executable):
        raise RuntimeError('No built executable at %s' % executable)

    catalogues = []
    po_dir = os.path.join(REPO, 'po')
    for name in sorted(os.listdir(po_dir)):
        if not name.endswith('.po'):
            continue
        language = name[:-3]
        catalogue = os.path.join(build_dir, 'po', language, 'LC_MESSAGES', 'aegisub.mo')
        if not os.path.isfile(catalogue):
            raise RuntimeError('No compiled catalogue at %s' % catalogue)
        catalogues.append((language, catalogue))

    os.makedirs(os.path.dirname(output), exist_ok=True)
    with zipfile.ZipFile(output, 'w', zipfile.ZIP_DEFLATED) as archive:
        archive.write(executable, 'aegisub.exe')
        for language, catalogue in catalogues:
            archive.write(catalogue, 'locale/%s/LC_MESSAGES/aegisub.mo' % language)

    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    try:
        output = package_update(args.build_dir, args.output)
    except (OSError, RuntimeError) as error:
        print('ERROR: %s' % error, file=sys.stderr)
        return 1
    print('Created %s' % output)
    return 0


if __name__ == '__main__':
    sys.exit(main())
