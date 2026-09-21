# Keep JNI bindings and public LibretroDroid APIs
-keep class com.swordfish.libretrodroid.** { *; }
-keepclassmembers class * {
    native <methods>;
}
