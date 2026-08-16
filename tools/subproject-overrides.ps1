# Replacement source trees for the directories the wraps name.
#
# Several of the extracted subprojects carry an ACL that locks this account out entirely:
# they cannot be read, renamed or deleted without taking ownership first. The copies below
# stand in for them. Both build-aegisub.ps1 and fix-subproject-permissions.ps1 read this
# file, so the mapping only has to be corrected in one place.
#
# Once fix-subproject-permissions.ps1 has put the copies under the names the wraps expect,
# the build stops consulting this list on its own: it only overrides a wrap whose own
# directory it cannot read.

[ordered]@{
    'boost.wrap'    = 'boost_1_83_0-codex-20260815'
    'curl.wrap'     = 'curl-8.12.1-aegisub'
    'gtest.wrap'    = 'googletest-1.17.0-aegisub'
    'hunspell.wrap' = 'hunspell-1.7.2-aegisub'
    'icu.wrap'      = 'icu-aegisub'
    'libpng.wrap'   = 'libpng-1.6.37-codex-20260815'
    'luajit.wrap'   = 'luajit-2.1.1720049189-aegisub'
    'uchardet.wrap' = 'uchardet-0.0.8-aegisub'
    'zlib.wrap'     = 'zlib-1.3.1-aegisub'
}
