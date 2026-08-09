"""Publish the contents of the release folder to the update location.

The destination comes from release.config.json. A local path or a UNC share is
copied; an sftp://, ftp:// or ftps:// URL is handed to curl, which ships with
Windows. The secret is never stored: it is read from MUTEKI_UPLOAD_SECRET or
asked for.
"""

import getpass
import os
import shutil
import subprocess
import sys

import release_common as common

REMOTE_PREFIXES = ('sftp://', 'ftp://', 'ftps://')


def collect(release_dir):
    """Files to publish: the zips and every changelog, with their relative names."""
    if not os.path.isdir(release_dir):
        common.fail('Release folder not found: %s' % release_dir)
    files = []
    for name in sorted(os.listdir(release_dir)):
        path = os.path.join(release_dir, name)
        if os.path.isfile(path) and name.lower().endswith(('.zip', '.txt')):
            files.append((path, name))
    changelog_dir = os.path.join(release_dir, 'changelog')
    if os.path.isdir(changelog_dir):
        for name in sorted(os.listdir(changelog_dir)):
            path = os.path.join(changelog_dir, name)
            if os.path.isfile(path) and name.lower().endswith('.txt'):
                files.append((path, 'changelog/%s' % name))
    if not files:
        common.fail('Nothing to upload in %s' % release_dir)
    return files


def upload_local(destination, files):
    for path, name in files:
        target = os.path.join(destination, name.replace('/', os.sep))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        print('  %s' % name)
        shutil.copy2(path, target)


def upload_remote(destination, user, files):
    if not shutil.which('curl'):
        common.fail('curl was not found; it is needed for a remote upload.')
    secret = os.environ.get('MUTEKI_UPLOAD_SECRET', '')
    if not secret:
        secret = getpass.getpass('Password for %s: ' % (user or destination))
    if not secret:
        common.fail('No password, nothing uploaded.')

    base = destination.rstrip('/')
    failures = []
    for path, name in files:
        url = '%s/%s' % (base, name)
        command = ['curl', '--fail', '--silent', '--show-error', '--ftp-create-dirs',
                   '--upload-file', path, url]
        if user:
            command += ['--user', '%s:%s' % (user, secret)]
        print('  %s' % name)
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            failures.append('%s: %s' % (name, result.stderr.strip()))
    if failures:
        print('\nSome files did not upload:')
        for failure in failures:
            print('  %s' % failure)
        sys.exit(1)


def main():
    config = common.load_config()
    destination = config['upload_destination']
    if 'CHANGE-ME' in destination:
        common.fail('Set upload_destination in %s first.' % common.CONFIG_PATH)

    files = collect(config['release_dir'])
    print('Uploading %d files from %s to %s\n' % (len(files), config['release_dir'], destination))
    if not common.ask_yes_no('Continue?'):
        print('Cancelled.')
        return

    if destination.startswith(REMOTE_PREFIXES):
        upload_remote(destination, config.get('upload_user', ''), files)
    else:
        upload_local(destination, files)
    print('\nDone.')


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
