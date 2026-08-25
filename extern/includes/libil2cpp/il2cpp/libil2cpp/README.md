# libil2cpp bundled headers

This directory contains the bundled libil2cpp headers required for building.

Since libil2cpp is not available on GitHub or qpackages.com, these headers must be obtained from:
1. Your local Quest build environment
2. The Il2CppQuestTypePatching repository's extern folder
3. Manual download from a private source

To use this mod, you need to populate this directory with the actual libil2cpp headers.

Expected structure:
- il2cpp/libil2cpp/ - core il2cpp headers
- il2cpp/external/baselib/Include/ - baselib headers  
- il2cpp/external/baselib/Platforms/Android/Include/ - Android-specific baselib headers
