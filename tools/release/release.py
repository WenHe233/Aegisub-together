"""Build and publish a release.

Three steps, each of which skips whatever is already done:

1. build Aegisub and compile every catalogue
2. pack aegisub.exe plus locale/ into
   <release_dir>/aegisub-nyaa-edition-v<version>-update.zip
3. optionally publish the package as a GitHub release
"""

import io
import os
import shutil
import sys
import zipfile

import release_common as common


SOURCE_DIRECTORIES = ('src', 'libaegisub', 'po')
SOURCE_SUFFIXES = ('.cpp', '.h', '.hpp', '.po', '.json', '.build')


def newest_source_change(build_dir):
    """(path, mtime) of the most recently touched source file, or None.

    Only the things that end up in the release: the code, the catalogues and the
    build definitions. What is checked out under subprojects is left alone, partly
    because it does not change between releases and partly because parts of it are
    not readable by every account on this machine.
    """
    newest = None
    for directory in SOURCE_DIRECTORIES:
        root = os.path.join(common.REPO, directory)
        for base, _, files in os.walk(root):
            if build_dir in base:
                continue
            for name in files:
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(base, name)
                try:
                    stamp = os.path.getmtime(path)
                except OSError:
                    continue
                if newest is None or stamp > newest[1]:
                    newest = (path, stamp)
    for name in ('meson.build', 'meson_options.txt'):
        path = os.path.join(common.REPO, name)
        if os.path.exists(path):
            stamp = os.path.getmtime(path)
            if newest is None or stamp > newest[1]:
                newest = (path, stamp)
    return newest


def build_needed(config):
    """Whether to build, and why. A build relinks an 85 MB executable every time
    because the version header is regenerated on every run, so it is worth
    skipping when there is provably nothing new to put in it.

    The changelog is not part of this: it is not compiled and not packed, so
    translating it changes nothing a build could produce.
    """
    executable = os.path.join(common.REPO, config['build_dir'], 'aegisub.exe')
    if not os.path.exists(executable):
        return True, 'there is no built executable yet'

    built = os.path.getmtime(executable)
    newest = newest_source_change(config['build_dir'])
    if newest and newest[1] > built:
        return True, '%s is newer than the executable' % os.path.relpath(newest[0], common.REPO)

    return False, 'no new translations and nothing newer than the executable'


def pack(config, version):
    build_dir = os.path.join(common.REPO, config['build_dir'])
    release_dir = config['release_dir']
    os.makedirs(release_dir, exist_ok=True)
    archive = os.path.join(release_dir, common.release_asset_name(config, version))

    executable = os.path.join(build_dir, 'aegisub.exe')
    if not os.path.exists(executable):
        common.fail('No built executable at %s' % executable)

    print('Packing %s' % archive)
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zip_file:
        zip_file.write(executable, 'aegisub.exe')
        for language in common.po_languages():
            source = os.path.join(common.REPO, 'po', '%s.po' % language)
            catalogue = os.path.join(build_dir, 'po', language, 'LC_MESSAGES', 'aegisub.mo')

            # A build compiles every catalogue, so normally the file is already here
            # and current. Compiling it costs a whole ninja invocation, which is why
            # it is only done when there is no other way to get the file.
            fresh = (os.path.exists(catalogue) and
                     os.path.getmtime(catalogue) >= os.path.getmtime(source))
            if not fresh:
                catalogue = common.build_catalogue(config, language)

            if not catalogue or not os.path.exists(catalogue):
                print('  %-12s catalogue failed to compile, left out' % language)
                continue
            zip_file.write(catalogue, 'locale/%s/LC_MESSAGES/aegisub.mo' % language)
    return archive


def main():
    config = common.load_config()
    source_changelog = os.path.join(config['changelog_dir'],
                                    '%s.txt' % config['release_language'])
    version = common.newest_changelog_version(source_changelog)
    print('Releasing version %s\n' % version)

    needed, reason = build_needed(config)
    if needed:
        print('Building because %s.' % reason)
        common.run_build(config)
    else:
        print('Skipping the build: %s.' % reason)

    archive = pack(config, version)

    print('')
    if common.ask_yes_no('Publish %s as a GitHub release?' % version):
        token = common.github_token()
        repo = config['github_repo']

        # Rebuilding a version that is already out is the normal case for a fix, so
        # ask rather than silently leaving the old package in place.
        _, assets = common.github_release_id(token, repo, 'v%s' % version)
        replace = False
        if os.path.basename(archive) in assets:
            replace = common.ask_yes_no(
                '%s is already published. Replace the attached package and notes?'
                % version)
            if not replace:
                print('Left as it is.')

        common.publish_release(token, repo, version, common.release_name(config, version), archive,
                               release_notes(config, version), replace)
        print('\nDone. Commit changelog/ so the program can see the new release.')
    else:
        print('\nDone. %s is built but not published.' % archive)


def release_notes(config, version):
    """The release's English changelog section, as the GitHub notes."""
    source = os.path.join(config['changelog_dir'], '%s.txt' % config['release_language'])
    for release_version, _, body in common.parse_changelog(source):
        if release_version == version:
            # The first line is the package address, which the GitHub page shows
            # anyway as the attached file.
            lines = [line for line in body if line.strip()]
            return '\n'.join(lines[1:]) if lines else ''
    return ''


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
