& "$PSScriptRoot/build.ps1"

adb shell am force-stop com.beatgames.beatsaber

adb push libs/arm64-v8a/libconditional-dependencies-test.so /sdcard/Android/data/com.beatgames.beatsaber/files/mods/libconditional-dependencies-test.so
adb push libs/arm64-v8a/libconditional-dependencies-test2.so /sdcard/Android/data/com.beatgames.beatsaber/files/mods/libconditional-dependencies-test2.so
