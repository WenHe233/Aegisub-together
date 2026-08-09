"""Build a publishable release.

Four steps, each of which skips whatever is already done:

1. translate msgids that po/hu.po has and another catalogue does not
2. translate the releases that changelog/hu.txt has and a language file does not
3. build Aegisub and compile every catalogue
4. pack aegisub.exe plus locale/ into <release_dir>/aegisub-v<version>.zip
"""

import io
import os
import re
import shutil
import sys
import zipfile

import release_common as common

# What msgfmt understands as a printf conversion. Only entries that really
# contain one may carry the c-format flag; on anything else msgfmt rejects the
# catalogue over a stray per cent sign.
PLACEHOLDER = re.compile(
    r'%(?:\d+\$)?[-+ #0]*[\d.*]*(?:hh|h|ll|l|L|z|j|t)?[diouxXeEfgGaAcspn%]')

# msgfmt drops fuzzy entries, so a machine translation marked fuzzy would never
# reach the program. The entries go in live and carry a translator comment
# instead, which is what a later reviewer can search for.
MACHINE_COMMENT = '# machine translation from Hungarian, not reviewed yet'

PO_INSTRUCTIONS = (
    'You translate strings of a subtitle editor user interface from Hungarian into '
    '%s. Reply with one translation per input line, in the same order, one per line, '
    'and nothing else: no numbering, no quotes, no commentary. Keep every printf '
    'placeholder (%%s, %%d, %%zu, %%.1f and so on), every escape (\\n, \\t), every '
    'keyboard accelerator marker (&) and any ASS override tag exactly as they appear. '
    'Keep the translation short enough for a button or a label. If a line is a '
    'product name, a file name or a command name, leave it as it is. '
    # The batches are translated independently, so without these two rules the
    # same catalogue ends up mixing forms of address and mixing "AI" with the
    # local word for it.
    'Address the user the same way throughout, with the form a commercial '
    'application would use in that language. Translate "AI" with the usual '
    'term of that language and then use that one term everywhere.'
)

CHANGELOG_INSTRUCTIONS = (
    'You translate the changelog of a subtitle editor from Hungarian into %s. Keep '
    'the layout byte for byte: the same line breaks, the same leading dashes and '
    'indentation, the same URLs, version numbers, file names, option names, command '
    'names and ASS tags. Translate the release header word for "version" into %s but '
    'keep the number and the trailing --- marker. Translate only prose. Reply with '
    'the translated text and nothing else.'
)


def suspicious(source, result):
    """Whether a translated line looks like the model gave up on it.

    Both of these were found in a finished run: strings handed back as the
    Hungarian they came from, and menu labels that lost their '&' accelerator.
    They are cheap to spot and cheap to ask again for, one line at a time.
    """
    if not result.strip():
        return True
    if result == source and len(source) > 12:
        return True
    if '&' in source and '&' not in result:
        return True
    return False


def translate_lines(key, model, language, lines):
    """Translate a batch of UI strings, keeping the one-per-line contract."""
    payload = '\n'.join(lines)
    answer = common.openai_text(key, model, PO_INSTRUCTIONS % language, payload)
    translated = answer.split('\n')
    if len(translated) != len(lines):
        # A mismatched batch cannot be matched up safely, so translate one by one.
        translated = []
        for line in lines:
            single = common.openai_text(key, model, PO_INSTRUCTIONS % language, line)
            translated.append(single.split('\n')[0])

    translated = [line.strip() for line in translated]

    for index, (source, result) in enumerate(zip(lines, translated)):
        if not suspicious(source, result):
            continue
        try:
            retry = common.openai_text(key, model, PO_INSTRUCTIONS % language + ' ' +
                'This line was returned untranslated or without its & marker last '
                'time. Translate it into the target language and keep the & exactly '
                'where the source has it.', source).split('\n')[0].strip()
        except Exception:
            continue
        # Only worth taking if the second answer is actually better.
        if retry and not suspicious(source, retry):
            translated[index] = retry

    return translated


def translate_catalogues(config, key):
    """Fill in what the other catalogues are missing. Returns the languages whose
    file was actually written, which is what decides whether a build is needed."""
    source = os.path.join(common.REPO, 'po', '%s.po' % config['source_language'])
    _, source_entries = common.parse_po(source)
    source_map = {}
    order = []
    for comments, msgid, msgstr in source_entries:
        if not msgid or not msgstr:
            continue
        if msgid in source_map:
            continue
        source_map[msgid] = (comments, msgstr)
        order.append(msgid)

    written = []
    for language in common.po_languages():
        if language == config['source_language']:
            continue
        path = os.path.join(common.REPO, 'po', '%s.po' % language)
        # Obsolete '#~' entries count: gettext treats them as taking the msgid, so
        # translating one again gives msgfmt two definitions and it rejects the file.
        known = common.taken_msgids(path)
        missing = [msgid for msgid in order if msgid not in known]
        if not missing:
            print('  %-12s up to date' % language)
            continue

        print('  %-12s %d new strings' % (language, len(missing)))
        blocks = []
        batch_size = 20
        for start in range(0, len(missing), batch_size):
            batch = missing[start:start + batch_size]
            # The Hungarian text is the source, not the English msgid: it is the one
            # that has actually been reviewed.
            hungarian = [source_map[msgid][1] for msgid in batch]
            try:
                translated = translate_lines(key, config['openai_model'], language, hungarian)
            except Exception as error:
                print('    batch failed (%s), leaving the rest for the next run' % error)
                break
            for msgid, text in zip(batch, translated):
                comments = [line for line in source_map[msgid][0] if line.startswith('#:')]
                block = MACHINE_COMMENT
                block += '\n' + ('\n'.join(comments) if comments else '#: src')
                if PLACEHOLDER.search(msgid):
                    block += '\n#, c-format'
                block += '\nmsgid %s\nmsgstr %s\n' % (common.quote_po(msgid), common.quote_po(text))
                blocks.append(block)
            print('    %d/%d' % (min(start + batch_size, len(missing)), len(missing)))
        if blocks:
            common.append_po_entries(path, blocks)
            written.append(language)

    return written


def translate_changelogs(config, key):
    directory = config['changelog_dir']
    source = os.path.join(directory, '%s.txt' % config['source_language'])
    if not os.path.exists(source):
        common.fail('No source changelog at %s' % source)
    source_releases = common.parse_changelog(source)

    languages = [config['fallback_language']] + [
        language for language in common.po_languages()
        if language not in (config['source_language'], config['fallback_language'])]

    for language in languages:
        path = os.path.join(directory, '%s.txt' % language)
        existing = common.parse_changelog(path) if os.path.exists(path) else []
        have = set(version for version, _, _ in existing)
        missing = [release for release in source_releases if release[0] not in have]
        if not missing:
            print('  %-12s up to date' % language)
            continue

        print('  %-12s %d new releases' % (language, len(missing)))
        translated = []
        for version, header, body in missing:
            block = '\n'.join([header] + body).rstrip()
            try:
                text = common.openai_text(key, config['openai_model'],
                                          CHANGELOG_INSTRUCTIONS % (language, language), block)
            except Exception as error:
                print('    %s failed (%s), leaving the rest for the next run' % (version, error))
                break
            lines = text.split('\n')
            # The header has to come back in a shape the release list can read, or
            # the release looks absent on the next run and gets translated again.
            if not lines or common.header_version(lines[0]) != version:
                print('    %s: header came back as %r, keeping the source header'
                      % (version, lines[0] if lines else ''))
                lines = [header] + (lines[1:] if lines else [])
            translated.append((version, lines[0], lines[1:]))
        if not translated:
            continue

        # Newest first, matching the source order.
        merged = []
        by_version = {version: release for release in translated for version in [release[0]]}
        for version, _, _ in source_releases:
            if version in by_version:
                merged.append(by_version[version])
            else:
                for release in existing:
                    if release[0] == version:
                        merged.append(release)
                        break
        common.write_changelog(path, merged)


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


def build_needed(config, translated_languages):
    """Whether to build, and why. A build relinks an 85 MB executable every time
    because the version header is regenerated on every run, so it is worth
    skipping when there is provably nothing new to put in it.

    The changelog is not part of this: it is not compiled and not packed, so
    translating it changes nothing a build could produce.
    """
    if translated_languages:
        return True, '%d catalogue(s) changed: %s' % (
            len(translated_languages), ' '.join(translated_languages))

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
    archive = os.path.join(release_dir, 'aegisub-v%s.zip' % version)

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
                                    '%s.txt' % config['source_language'])
    version = common.newest_changelog_version(source_changelog)
    print('Releasing version %s\n' % version)

    key = common.openai_key(config)

    print('Translating catalogues:')
    translated = translate_catalogues(config, key)
    print('\nTranslating changelogs:')
    translate_changelogs(config, key)

    print('')
    needed, reason = build_needed(config, translated)
    if needed:
        print('Building because %s.' % reason)
        common.run_build(config)
    else:
        print('Skipping the build: %s.' % reason)

    archive = pack(config, version)
    print('\nDone. %s is ready to upload.' % archive)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
