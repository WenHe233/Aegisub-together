"""Put the freshly built Aegisub into the portable folder for immediate use.

Copies the built aegisub.exe and the Hungarian catalogue into
C:\\aegisub-portable. Refuses to run while that copy is open, because Windows
would either lock the file or leave a half-updated install behind.
"""

import os
import shutil
import sys

import release_common as common


def main():
    config = common.load_config()
    portable = config['portable_dir']
    build_dir = os.path.join(common.REPO, config['build_dir'])
    language = config['source_language']

    source_exe = os.path.join(build_dir, 'aegisub.exe')
    target_exe = os.path.join(portable, 'aegisub.exe')
    if not os.path.exists(source_exe):
        common.fail('No built executable at %s - build first.' % source_exe)
    if not os.path.isdir(portable):
        common.fail('Portable folder not found: %s' % portable)

    # Any running copy blocks this: the portable file would be locked, and a build
    # tree copy still running means the exe about to be copied is the stale one.
    common.require_not_running(target_exe)

    print('Compiling the %s catalogue...' % language)
    catalogue = common.build_catalogue(config, language)
    if not catalogue:
        common.fail('The %s catalogue could not be compiled.' % language)

    print('%s  ->  %s' % (source_exe, target_exe))
    shutil.copy2(source_exe, target_exe)
    pdb = os.path.join(build_dir, 'aegisub.pdb')
    if os.path.exists(pdb):
        shutil.copy2(pdb, os.path.join(portable, 'aegisub.pdb'))

    target_dir = os.path.join(portable, 'locale', language, 'LC_MESSAGES')
    os.makedirs(target_dir, exist_ok=True)
    target_mo = os.path.join(target_dir, 'aegisub.mo')
    print('%s  ->  %s' % (catalogue, target_mo))
    shutil.copy2(catalogue, target_mo)

    print('\nDone. %s is up to date.' % portable)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
