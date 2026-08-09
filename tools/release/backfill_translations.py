"""One-off: push the existing Hungarian text out to every other language.

Runs only the two translation passes that release.cmd also performs - the
catalogues and the changelog - without building or packing anything. Meant for
catching up the backlog once; after that release.cmd keeps things current,
because both passes only ever touch what is missing.

Safe to stop and re-run: whatever already landed is skipped.
"""

import os
import sys

import release_common as common
import release


def main():
    config = common.load_config()
    languages = [name for name in common.po_languages()
                 if name != config['source_language']]
    source_changelog = os.path.join(config['changelog_dir'],
                                    '%s.txt' % config['source_language'])

    print('Source language: %s' % config['source_language'])
    print('Target languages: %d' % len(languages))
    print('Catalogues: %s' % os.path.join(common.REPO, 'po'))
    print('Changelogs: %s' % config['changelog_dir'])

    if not os.path.exists(source_changelog):
        common.fail('No source changelog at %s' % source_changelog)

    # Say how much work it is before spending anything on it.
    source_po = os.path.join(common.REPO, 'po', '%s.po' % config['source_language'])
    _, source_entries = common.parse_po(source_po)
    source_ids = [msgid for _, msgid, msgstr in source_entries if msgid and msgstr]
    total_strings = 0
    for language in languages:
        _, entries = common.parse_po(os.path.join(common.REPO, 'po', '%s.po' % language))
        known = set(msgid for _, msgid, _ in entries)
        total_strings += sum(1 for msgid in source_ids if msgid not in known)

    source_versions = common.changelog_versions(source_changelog)
    total_releases = 0
    for language in [config['fallback_language']] + languages:
        if language == config['fallback_language'] or language != config['source_language']:
            path = os.path.join(config['changelog_dir'], '%s.txt' % language)
            have = set(common.changelog_versions(path))
            total_releases += sum(1 for version in source_versions if version not in have)

    print('\nStill missing: %d strings and %d changelog releases.' % (total_strings, total_releases))
    if not total_strings and not total_releases:
        print('Nothing to do.')
        return
    print('This calls the OpenAI API and costs money.')
    if not common.ask_yes_no('Start?'):
        print('Cancelled.')
        return

    key = common.openai_key(config)

    print('\nCatalogues:')
    release.translate_catalogues(config, key)
    print('\nChangelogs:')
    release.translate_changelogs(config, key)

    print('\nDone. Review the "#, fuzzy" entries in po/*.po before releasing.')


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    main()
