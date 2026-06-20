// SPDX-License-Identifier: MIT
// Copyright (c) 2026 4noha

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.fournoha.wakeandroidtether"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.fournoha.wakeandroidtether"
        minSdk = 31
        targetSdk = 34
        versionCode = 4
        versionName = "0.3.0"
    }

    buildTypes {
        debug {
            // No CI signing / distribution in the public build; build and
            // sideload with `./gradlew :app:assembleDebug` then
            // `adb install app/build/outputs/apk/debug/app-debug.apk`.
        }
        release {
            isMinifyEnabled = false
            // To produce a release APK you'll need to provide your own
            // signingConfig — see https://developer.android.com/build/building-cmdline
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
    implementation("androidx.lifecycle:lifecycle-service:2.8.4")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.8.4")

    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")
}
