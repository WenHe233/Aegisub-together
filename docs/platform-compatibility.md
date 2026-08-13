# Platform compatibility policy

Aegisub supports Windows, Linux, and macOS. Every new feature and every change to an existing implementation must preserve a functional implementation on all three platforms.

Windows may use the preferred or more deeply native implementation when this improves behavior, integration, or performance. Windows-specific code must be kept behind explicit platform guards such as `__WXMSW__`, with equivalent portable wxWidgets code or dedicated Linux and macOS implementations in the other platform branches.

Shared source files and headers must not include Windows SDK headers or use Win32, GDI, COM, DirectSound, or other Windows-only APIs unconditionally. Linux- and macOS-specific headers, APIs, and Meson dependencies must likewise remain conditional.

Public and shared headers must be self-contained. Code must use the required portable include or a sufficient forward declaration rather than depending on precompiled headers or unrelated translation units to supply declarations indirectly.

Prefer cross-platform wxWidgets, standard C++, OpenGL, and existing project abstractions for shared behavior. Native platform APIs are appropriate when they provide material value or when no adequate portable abstraction exists, provided every supported platform retains a working implementation.

Platform-specific changes must pass the complete Windows build and, when available, a Linux build with precompiled headers disabled. The GitHub Actions configurations for Linux, Intel macOS, and Apple Silicon macOS must remain enabled so all platform branches are compiled before release.
