# Keep JNI bindings and public LibretroDroid APIs
-keep class com.omnidroid.libretrodroid.** { *; }
-keepclassmembers class * {
    native <methods>;
}
