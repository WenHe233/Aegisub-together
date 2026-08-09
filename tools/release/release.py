"""Build a publishable release.

Four steps, each of which skips whatever is already done:

1. translate msgids that po/hu.po has and another catalogue does not
2. translate the releases that changelog/hu.txt has and a language file does not
3. build Aegisub and compile every catalogue
4. pack aegisub.exe plus locale/ into <release_dir>/aegisub-v<version>.zip
"""

import io
import os
import shutil
import sys
import zipfile

import release_common as common

PO_INSTRUCTIONS = (
    'You translate strings of a subtitle editor user interface from Hungarian into '
    '%s. Reply with one translation per input line, in the same order, one per line, '
    'and nothing else: no numbering, no quotes, no commentary. Keep every printf '
    'placeholder (%%s, %%d, %%zu, %%.1f and so on), every escape (\\n, \\t), every '
    'keyboard accelerator marker (&) and any ASS override tag exactly as they appear. '
    'Keep the translation short enough for a button or a label. If a line is a '
    'product name, a file name or a command name, leave it as it is.'
)

CHANGELOG_INSTRUCTIONS = (
    'You translate the changelog of a subtitle editor from Hungarian into %s. Keep '
    'the layout byte for byte: the same line breaks, the same leading dashes and '
    'indentation, the same URLs, version numbers, file names, option names, command '
    'names and ASS tags. Translate the release header word for "version" into %s but '
    'keep the number and the trailing --- marker. Translate only prose. Reply with '
    'the translated text and nothing else.'
)


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
    return [line.strip() for line in translated]


def translate_catalogues(config, key):
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

    for language in common.po_languages():
        if language == config['source_language']:
            continue
        path = os.path.join(common.REPO, 'po', '%s.po' % language)
        _, entries = common.parse_po(path)
        known = set(msgid for _, msgid, _ in entries)
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
                block = '\n'.join(comments) if comments else '#: src'
                if '%' in msgid:
                    block += '\n#, fuzzy, c-format'
                else:
                    block += '\n#, fuzzy'
                block += '\nmsgid %s\nmsgstr %s\n' % (common.quote_po(msgid), common.quote_po(text))
                blocks.append(block)
            print('    %d/%d' % (min(start + batch_size, len(missing)), len(missing)))
        if blocks:
            common.append_po_entries(path, blocks)


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
            catalogue = common.build_catalogue(config, language)
            if not catalogue:
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

    key = common.openai_key()

    print('Translating catalogues:')
    translate_catalogues(config, key)
    print('\nTranslating changelogs:')
    translate_changelogs(config, key)

    print('')
    common.run_build(config)
    archive = pack(config, version)
    print('\nDone. %s is ready to upload.' % archive)


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
