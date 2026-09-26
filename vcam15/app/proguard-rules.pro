# Keep JNI method names
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.nvshen.chmp4.VCamManager { *; }
-keep class com.nvshen.chmp4.LicenseManager { *; }
