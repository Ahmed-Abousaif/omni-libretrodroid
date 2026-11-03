plugins {
    id("com.android.library")
    id("kotlin-android")
}

android {
    namespace = "com.omnidroid.libretrodroid"

    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")

        externalNativeBuild {
            cmake {
                arguments("-DANDROID_STL=c++_shared", "-DANDROID_PLATFORM=android-24")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }
}

dependencies {
    implementation(fileTree(mapOf("dir" to "libs", "include" to listOf("*.jar"))))
    implementation(deps.libs.androidx.lifecycle.runtime)
    implementation(deps.libs.androidx.lifecycle.commonJava8)
    implementation(deps.libs.androidx.ktx.coreKtx)
    implementation(deps.libs.kotlinxCoroutinesAndroid)
    implementation(kotlin(deps.libs.kotlin.stdlib))
}
