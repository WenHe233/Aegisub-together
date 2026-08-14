"""Build GitHub release notes from one English changelog entry."""

import argparse
import io
import os
import sys

import release_common as common


def notes_for_version(changelog, version):
    for release_version, _, body in common.parse_changelog(changelog):
        if release_version != version:
            continue

        lines = list(body)
        while lines and not lines[0].strip():
            lines.pop(0)
        if not lines or not lines[0].strip().startswith('https://'):
            raise RuntimeError(
                'Version %s has no HTTPS update-package URL in %s' %
                (version, changelog))
        lines.pop(0)
        while lines and not lines[0].strip():
            lines.pop(0)
        while lines and not lines[-1].strip():
            lines.pop()
        if not lines:
            raise RuntimeError('Version %s has no English changelog text' % version)
        return "## What's Changed\n%s\n" % '\n'.join(lines)

    raise RuntimeError('Version %s was not found in %s' % (version, changelog))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--changelog', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()

    try:
        notes = notes_for_version(args.changelog, args.version)
        output = os.path.abspath(args.output)
        directory = os.path.dirname(output)
        if directory:
            os.makedirs(directory, exist_ok=True)
        with io.open(output, 'w', encoding='utf-8', newline='\n') as handle:
            handle.write(notes)
    except (OSError, RuntimeError) as error:
        print('ERROR: %s' % error, file=sys.stderr)
        return 1

    print('Created %s' % output)
    return 0


if __name__ == '__main__':
    sys.exit(main())
