"""Build and start a cross-platform release.

Three steps, each of which skips whatever is already done:

1. build Aegisub and compile every catalogue
2. pack aegisub.exe plus locale/ into
   <release_dir>/Aegisub-nyaa-edition-v<version>-update.zip
3. optionally create and push the version tag which makes GitHub Actions build
   and publish every platform package
"""

import os
import subprocess
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


def git_result(*args):
    return subprocess.run(
        ['git', '-C', common.REPO] + list(args),
        capture_output=True, text=True, encoding='utf-8', errors='replace')


def git_output(*args):
    result = git_result(*args)
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        common.fail('Git failed while running "git %s": %s' % (' '.join(args), detail))
    return result.stdout.strip()


def start_cross_platform_release(config, version):
    """Validate and push the tag consumed by the cross-platform release workflow."""
    tag = 'v%s' % version
    dirty = git_output('status', '--porcelain', '--untracked-files=normal')
    if dirty:
        common.fail(
            'Commit and push every release change before creating %s.\n%s' %
            (tag, dirty))

    git_output('fetch', '--quiet', 'origin')
    branch = git_output('branch', '--show-current')
    if not branch:
        common.fail('A release tag cannot be created from a detached HEAD.')

    default_ref = git_result('symbolic-ref', '--quiet', 'refs/remotes/origin/HEAD')
    if default_ref.returncode == 0:
        default_branch = default_ref.stdout.strip().split('/')[-1]
        if branch != default_branch:
            common.fail(
                '%s is not the default branch (%s). Switch to %s before releasing.' %
                (branch, default_branch, default_branch))

    head = git_output('rev-parse', 'HEAD')
    origin_branch = 'refs/remotes/origin/%s' % branch
    pushed = git_result('rev-parse', '--verify', origin_branch)
    if pushed.returncode:
        common.fail('The current branch has not been pushed to origin. Push it before releasing.')
    if head != pushed.stdout.strip():
        common.fail(
            'The current commit is not the commit on origin/%s. '
            'Push the branch before creating %s.' % (branch, tag))

    if git_result('show-ref', '--verify', '--quiet', 'refs/tags/%s' % tag).returncode == 0:
        common.fail('%s already exists locally. Use a new version tag.' % tag)
    if git_result('ls-remote', '--exit-code', '--tags', 'origin',
                  'refs/tags/%s' % tag).returncode == 0:
        common.fail(
            '%s already exists on GitHub. Pushing it again would not start a new '
            'workflow; use a new version tag.' % tag)

    git_output('tag', '--annotate', tag, '--message', common.release_name(config, version))
    pushed = git_result('push', 'origin', tag)
    if pushed.returncode:
        detail = (pushed.stderr or pushed.stdout).strip()
        common.fail(
            'The local %s tag was created, but pushing it failed: %s\n'
            'After fixing the problem, run: git push origin %s' % (tag, detail, tag))

    print('\n%s was pushed. GitHub Actions is now building all seven release assets.' % tag)
    print('Follow the build at https://github.com/%s/actions' % config['github_repo'])


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

    tag = 'v%s' % version
    print('\nThe local Windows updater package is ready: %s' % archive)
    print('The installer, portable ZIP, macOS DMGs, AppImage, tarball, and updater ZIP')
    print('are built and published by GitHub Actions after pushing %s.' % tag)
    print('For a fork, enable workflows once on https://github.com/%s/actions' %
          config['github_repo'])
    if common.ask_yes_no('Create and push %s now?' % tag):
        start_cross_platform_release(config, version)
    else:
        print('\nNo tag was pushed; no GitHub release was started.')


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
