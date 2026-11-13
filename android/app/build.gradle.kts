plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
    id("org.jetbrains.kotlin.plugin.serialization")
}

android {
    namespace = "local.wechatlite.mobile"
    compileSdk = 36

    defaultConfig {
        applicationId = "local.wechatlite.mobile"
        minSdk = 26
        targetSdk = 36
        versionCode = 9
        versionName = "0.1.8"
        // Shown in 设置 → 关于 so a screenshot identifies the build that is actually installed.
        // Android's app info only repeats the version number, which is not enough when several
        // rebuilds share it.
        buildConfigField("String", "BUILD_STAMP", "\"2026-10-26 guard\"")
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // The bundled P2P helper exists only for arm64. Declaring other ABIs would let the APK
        // install on a device where automatic connection can never work, so keep both in step.
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    packaging {
        resources.excludes += setOf("/META-INF/{AL2.0,LGPL2.1}")
        // `libwechat_p2p.so` is a genuine shared library loaded with System.loadLibrary, so the
        // default (uncompressed, loaded straight from the APK) is exactly right.
        jniLibs.useLegacyPackaging = false
    }
}

// AGP 9 ships built-in Kotlin support, so the kotlin-android plugin is not applied here.
kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2025.09.00")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.10.1")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    debugImplementation("androidx.compose.ui:ui-tooling")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
    // The platform `org.json` on the JVM unit-test classpath is a stub whose methods return null or
    // throw, so a serialisation test would fail for a reason unrelated to the code under test. The
    // real implementation makes the JSON round trip testable off-device.
    testImplementation("org.json:json:20240303")

    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-test:1.9.0")
}
