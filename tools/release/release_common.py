"""Shared bits for the release helpers: settings, paths, po and changelog parsing."""

import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
CONFIG_PATH = os.path.join(HERE, 'release.config.json')
# release.config.json is committed, so a key put there would end up in the repo.
# Keys live in this file instead, which .gitignore keeps out.
APIKEY_PATH = os.path.join(HERE, 'release.config.apikey.json')

DEFAULT_APIKEY = {
    'openai_api_key': '',
}

DEFAULT_CONFIG = {
    'build_dir': 'build-codex',
    'portable_dir': r'C:\aegisub-portable',
    # Both inside the repository now. The changelogs are tracked, because the program
    # reads them from GitHub; .releases is gitignored, since it only holds the zips.
    'release_dir': os.path.join(REPO, '.releases'),
    'changelog_dir': os.path.join(REPO, 'changelog'),
    # <owner>/<repo>. The releases the program downloads are this repository's
    # release assets, so the upload step needs to know which one.
    'github_repo': 'croni1012/Aegisub',
    'source_language': 'hu',
    'fallback_language': 'en',
    'release_language': 'en',
    'release_asset_template': 'Aegisub-nyaa-edition-v{version}-update.zip',
    'release_name_template': 'Aegisub-nyaa-edition-v{version}',
    'openai_model': 'gpt-5.6-terra',
    # Keys belong in release.config.apikey.json; this stays empty in the
    # committed file, and is only read as a fallback.
    'openai_api_key': '',
    'vcvars': r'D:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
}


def load_config():
    config = dict(DEFAULT_CONFIG)
    if os.path.exists(CONFIG_PATH):
        with io.open(CONFIG_PATH, encoding='utf-8-sig') as handle:
            config.update(json.load(handle))
    else:
        save_config(config)
        print('Created %s - check the paths in it before publishing.' % CONFIG_PATH)
    return config


def load_apikeys():
    """Read release.config.apikey.json, creating an empty one on first run."""
    if not os.path.exists(APIKEY_PATH):
        with io.open(APIKEY_PATH, 'w', encoding='utf-8', newline='\n') as handle:
            json.dump(DEFAULT_APIKEY, handle, indent=2)
            handle.write('\n')
        print('Created %s - put your OpenAI key in it.' % APIKEY_PATH)
        return dict(DEFAULT_APIKEY)
    with io.open(APIKEY_PATH, encoding='utf-8-sig') as handle:
        keys = dict(DEFAULT_APIKEY)
        keys.update(json.load(handle))
        return keys


def save_config(config):
    with io.open(CONFIG_PATH, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(config, handle, indent=2, ensure_ascii=False)
        handle.write('\n')


def fail(message):
    print('\nERROR: %s' % message)
    sys.exit(1)


def ask_yes_no(question):
    while True:
        try:
            answer = input('%s [y/n] ' % question).strip().lower()
        except EOFError:
            # No console to answer on: take the cautious option rather than loop.
            print('n')
            return False
        if answer in ('y', 'yes'):
            return True
        if answer in ('n', 'no'):
            return False


# ---------------------------------------------------------------- process state

def running_executables(path):
    """PIDs of processes running the given executable, via tasklist."""
    name = os.path.basename(path)
    try:
        output = subprocess.run(['tasklist', '/fi', 'imagename eq %s' % name, '/fo', 'csv', '/nh'],
                                capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    return [line for line in output.splitlines() if line.strip().startswith('"%s"' % name)]


def require_not_running(path):
    """tasklist matches on the image name, so this catches any copy of it."""
    if running_executables(path):
        fail('%s is running (any copy counts). Close it and start this script again.'
             % os.path.basename(path))


# ------------------------------------------------------------------- catalogues

def po_languages(repo=REPO):
    directory = os.path.join(repo, 'po')
    return sorted(name[:-3] for name in os.listdir(directory) if name.endswith('.po'))


ENTRY_SPLIT = re.compile(r'\n\s*\n')


def parse_po(path):
    """Return (header_text, [(comment_lines, msgid, msgstr)]) keeping order."""
    with io.open(path, encoding='utf-8') as handle:
        text = handle.read()
    blocks = ENTRY_SPLIT.split(text)
    header = blocks[0] if blocks else ''
    entries = []
    for block in blocks[1:]:
        lines = block.split('\n')
        comments = [line for line in lines if line.startswith('#')]
        msgid = None
        msgstr = None
        target = None
        for line in lines:
            if line.startswith('msgid '):
                msgid = unquote_po(line[len('msgid '):])
                target = 'id'
            elif line.startswith('msgstr '):
                msgstr = unquote_po(line[len('msgstr '):])
                target = 'str'
            elif line.startswith('"'):
                if target == 'id':
                    msgid += unquote_po(line)
                elif target == 'str':
                    msgstr += unquote_po(line)
        if msgid is not None and msgstr is not None:
            entries.append((comments, msgid, msgstr))
    return header, entries


def parse_obsolete_po(path):
    """Return [(msgid, msgstr)] for the entries the catalogue keeps under '#~'.

    gettext counts obsolete entries towards duplicate detection, so a msgid that
    only appears here is still taken: translating it again produces a second
    definition and msgfmt rejects the whole catalogue. Anything reading a
    catalogue to decide what is missing has to look at these too.
    """
    with io.open(path, encoding='utf-8') as handle:
        text = handle.read()
    entries = []
    for block in ENTRY_SPLIT.split(text):
        msgid = None
        msgstr = None
        target = None
        for line in block.split('\n'):
            if not line.startswith('#~'):
                continue
            line = line[2:].lstrip()
            if line.startswith('msgid '):
                msgid = unquote_po(line[len('msgid '):])
                target = 'id'
            elif line.startswith('msgstr '):
                msgstr = unquote_po(line[len('msgstr '):])
                target = 'str'
            elif line.startswith('"'):
                if target == 'id':
                    msgid += unquote_po(line)
                elif target == 'str':
                    msgstr += unquote_po(line)
        if msgid is not None and msgstr is not None:
            entries.append((msgid, msgstr))
    return entries


def taken_msgids(path):
    """Every msgid the catalogue already spends, live or obsolete."""
    _, entries = parse_po(path)
    taken = set(msgid for _, msgid, _ in entries if msgid)
    taken.update(msgid for msgid, _ in parse_obsolete_po(path))
    return taken


def unquote_po(value):
    value = value.strip()
    if len(value) >= 2 and value[0] == '"' and value[-1] == '"':
        value = value[1:-1]
    return value.replace('\\"', '"').replace('\\\\', '\\')


def quote_po(value):
    return '"%s"' % value.replace('\\', '\\\\').replace('"', '\\"')


def append_po_entries(path, blocks):
    """Append already formatted entry blocks to a catalogue."""
    with io.open(path, encoding='utf-8') as handle:
        text = handle.read()
    if not text.endswith('\n'):
        text += '\n'
    text += '\n' + '\n'.join(blocks)
    with io.open(path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)


# -------------------------------------------------------------------- changelog

def header_version(line):
    """The version a release header announces, or '' if it is not a header."""
    stripped = line.strip()
    if not stripped.endswith('---'):
        return ''
    numbered = [word for word in stripped[:-3].strip().split()
                if any(character.isdigit() for character in word)]
    return numbered[-1] if numbered else ''


def parse_changelog(path):
    """Return [(version, header_line, body_lines)] in file order.

    A release header is any line ending in '---' with a word containing a digit
    before the marker; the last such word is the version. Any word rather than
    the last one, because languages do not agree on where the word for "version"
    goes: Basque came back as "3.5.2 bertsioa ---" and a stricter rule silently
    dropped the release, which then looked missing and got translated twice.
    Keep in step with ParseChangelog in muteki_update.cpp.
    """
    with io.open(path, encoding='utf-8') as handle:
        lines = handle.read().split('\n')
    releases = []
    current = None
    for line in lines:
        version = header_version(line)
        if version:
            if current:
                releases.append(current)
            current = (version, line, [])
            continue
        if current:
            current[2].append(line)
    if current:
        releases.append(current)
    return releases


def changelog_versions(path):
    if not os.path.exists(path):
        return []
    return [version for version, _, _ in parse_changelog(path)]


def write_changelog(path, releases):
    parts = []
    for _, header, body in releases:
        block = [header] + body
        while block and not block[-1].strip():
            block.pop()
        parts.append('\n'.join(block))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with io.open(path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write('\n\n'.join(parts) + '\n')


# ----------------------------------------------------------------------- OpenAI

def openai_key(config=None):
    """From release.config.apikey.json first, then release.config.json, then the
    environment. Only the apikey file is kept out of git."""
    key = load_apikeys().get('openai_api_key', '').strip()
    if key:
        return key
    if config:
        key = str(config.get('openai_api_key', '')).strip()
        if key:
            print('Using the key from release.config.json. That file is committed, so '
                  'release.config.apikey.json is the safe place for it.')
            return key
    key = os.environ.get('OPENAI_API_KEY', '').strip()
    if key:
        return key
    fail('No OpenAI key. Put one in openai_api_key in %s.' % APIKEY_PATH)


def openai_text(key, model, instructions, user_text):
    """One plain-text Responses call. Returns the model's text."""
    import urllib.error
    import urllib.request

    body = json.dumps({
        'model': model,
        'instructions': instructions,
        'input': [{'role': 'user', 'content': user_text}],
        # Mechanical translation needs no deliberation, and reasoning tokens are
        # billed as output. The program's own requests ask for low effort too.
        'reasoning': {'effort': 'low'},
        'store': False,
        'max_output_tokens': 16000,
    }).encode('utf-8')
    request = urllib.request.Request('https://api.openai.com/v1/responses', data=body,
                                     headers={'Authorization': 'Bearer %s' % key,
                                              'Content-Type': 'application/json'})
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as error:
        detail = error.read().decode('utf-8', 'replace')
        raise RuntimeError('OpenAI HTTP %d: %s' % (error.code, detail[:400]))

    chunks = []
    for item in payload.get('output', []):
        for piece in item.get('content', []):
            if piece.get('type') in ('output_text', 'text') and piece.get('text'):
                chunks.append(piece['text'])
    text = ''.join(chunks).strip()
    if not text:
        raise RuntimeError('OpenAI returned no text.')
    return text


# ----------------------------------------------------------------------- GitHub

GITHUB_API = 'https://api.github.com'
GITHUB_UPLOADS = 'https://uploads.github.com'


def github_token():
    """From release.config.apikey.json, then the environment.

    A fine-grained token with "Contents: read and write" on the one repository is
    enough; nothing here needs more than that.
    """
    token = load_apikeys().get('github_token', '').strip()
    if token:
        return token
    for name in ('GH_TOKEN', 'GITHUB_TOKEN'):
        token = os.environ.get(name, '').strip()
        if token:
            return token
    fail('No GitHub token. Put one in github_token in %s.\n'
         'Create it at https://github.com/settings/personal-access-tokens with\n'
         '"Contents: read and write" on the release repository.' % APIKEY_PATH)


def github_request(token, method, url, data=None, content_type=None):
    import urllib.error
    import urllib.request

    headers = {
        'Authorization': 'Bearer %s' % token,
        'Accept': 'application/vnd.github+json',
        'X-GitHub-Api-Version': '2022-11-28',
        'User-Agent': 'muteki-release-script',
    }
    if content_type:
        headers['Content-Type'] = content_type

    request = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            body = response.read()
            return response.status, (json.loads(body) if body else {})
    except urllib.error.HTTPError as error:
        body = error.read().decode('utf-8', 'replace')
        try:
            return error.code, json.loads(body)
        except ValueError:
            return error.code, {'message': body[:400]}


def github_release_id(token, repo, tag):
    """(release id, {asset name: asset id}) for this tag, or (None, {}) if there is
    no release for it yet."""
    status, body = github_request(
        token, 'GET', '%s/repos/%s/releases/tags/%s' % (GITHUB_API, repo, tag))
    if status == 200:
        assets = {asset.get('name'): asset.get('id') for asset in body.get('assets', [])}
        return body.get('id'), assets
    if status == 404:
        return None, {}
    fail('GitHub returned %d looking up %s: %s' % (status, tag, body.get('message')))


def github_create_release(token, repo, tag, name, notes):
    status, body = github_request(
        token, 'POST', '%s/repos/%s/releases' % (GITHUB_API, repo),
        data=json.dumps({
            'tag_name': tag,
            'name': name,
            'body': notes,
            'draft': False,
            'prerelease': False,
        }).encode('utf-8'),
        content_type='application/json')
    if status not in (200, 201):
        fail('GitHub returned %d creating %s: %s' % (status, tag, body.get('message')))
    return body.get('id')


def github_upload_asset(token, repo, release_id, path):
    """Attach one file to a release. Read whole rather than streamed: the packages
    are tens of megabytes, which is nothing next to needing a chunked uploader."""
    with io.open(path, 'rb') as handle:
        payload = handle.read()

    name = os.path.basename(path)
    status, body = github_request(
        token, 'POST',
        '%s/repos/%s/releases/%s/assets?name=%s' % (GITHUB_UPLOADS, repo, release_id, name),
        data=payload, content_type='application/zip')
    if status not in (200, 201):
        fail('GitHub returned %d uploading %s: %s' % (status, name, body.get('message')))


def github_delete_asset(token, repo, asset_id):
    status, body = github_request(
        token, 'DELETE', '%s/repos/%s/releases/assets/%s' % (GITHUB_API, repo, asset_id))
    if status not in (200, 204):
        fail('GitHub returned %d deleting an asset: %s' % (status, body.get('message')))


def github_update_release(token, repo, release_id, name, notes):
    status, body = github_request(
        token, 'PATCH', '%s/repos/%s/releases/%s' % (GITHUB_API, repo, release_id),
        data=json.dumps({'name': name, 'body': notes}).encode('utf-8'),
        content_type='application/json')
    if status != 200:
        fail('GitHub returned %d updating the release metadata: %s' % (status, body.get('message')))


def release_asset_name(config, version):
    return config['release_asset_template'].format(version=version)


def release_name(config, version):
    return config['release_name_template'].format(version=version)


def publish_release(token, repo, version, name, package, notes, replace=False):
    """Make sure the release for this version exists and carries its package.

    Without `replace` an existing package is left alone, so the function is safe to
    run again after a failure part way through. With it, the old asset is deleted
    and the new one uploaded, and the notes are refreshed from the changelog -
    GitHub does not allow an asset to be overwritten in place, a same-name upload
    is refused, so replacing means deleting first.
    """
    tag = 'v%s' % version
    release_id, assets = github_release_id(token, repo, tag)
    if release_id is None:
        release_id = github_create_release(token, repo, tag, name, notes)
        print('    %s: release created' % tag)
    else:
        github_update_release(token, repo, release_id, name, notes)
        print('    %s: title and English notes updated' % tag)

    name = os.path.basename(package)
    if name in assets:
        if not replace:
            print('    %s: package already uploaded, left alone' % tag)
            return
        github_delete_asset(token, repo, assets[name])
        print('    %s: previous %s removed' % (tag, name))

    github_upload_asset(token, repo, release_id, package)
    print('    %s: %s uploaded' % (tag, name))


# ------------------------------------------------------------------------ build

def run_build(config):
    build_dir = config['build_dir']
    vcvars = config['vcvars']
    if not os.path.exists(vcvars):
        fail('Visual Studio environment script not found: %s (fix vcvars in release.config.json)' % vcvars)
    command = '"%s" >nul 2>&1 && cd /d "%s" && ninja -C "%s"' % (vcvars, REPO, build_dir)
    print('Building (%s)...' % build_dir)
    # shell=True hands the string to cmd untouched; passing it as an argv element
    # lets Python re-quote it and cmd then misreads the inner quotes.
    result = subprocess.run(command, shell=True)
    if result.returncode != 0:
        fail('The build failed.')


def build_catalogue(config, language):
    """Compile one .po into the build tree and return the .mo path."""
    target = 'po/%s/LC_MESSAGES/aegisub.mo' % language
    command = '"%s" >nul 2>&1 && cd /d "%s" && ninja -C "%s" %s' % (
        config['vcvars'], REPO, config['build_dir'], target)
    result = subprocess.run(command, shell=True, capture_output=True, text=True)
    path = os.path.join(REPO, config['build_dir'], 'po', language, 'LC_MESSAGES', 'aegisub.mo')
    if result.returncode != 0 or not os.path.exists(path):
        return None
    return path


def newest_changelog_version(path):
    """The version the zip is named after: the first release in the changelog."""
    versions = changelog_versions(path)
    if not versions:
        fail('No release found in %s' % path)
    return versions[0]
