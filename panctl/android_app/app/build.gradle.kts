plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("com.google.firebase.appdistribution")
}

// AppTester (Firebase App Distribution) 配信設定。audio-router-android と同じ流儀。
// 鍵 JSON はリポにコミットしない。env か local.properties で渡す。
// デフォルトは audio-router-android のものを流用 (同じ Firebase project)。
val fbAppId: String =
    System.getenv("FIREBASE_APP_ID")
        ?: (project.findProperty("firebaseAppId") as String?)
        ?: "1:282804505520:android:57a7dc1bcf1f3698bd2475"
val fbCreds: String =
    System.getenv("FIREBASE_APP_DISTRIBUTION_CREDS")
        ?: (project.findProperty("firebaseAppDistributionCreds") as String?)
        ?: "${System.getProperty("user.home")}/works/4noha-studio-family/audio-router-android/firebase-appdistribution.json"

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
            // ./gradlew :app:assembleDebug :app:appDistributionUploadDebug
            firebaseAppDistribution {
                appId = fbAppId
                artifactType = "APK"
                testers = "<your-email>"
                releaseNotesFile = "${rootDir}/release-notes.txt"
                serviceCredentialsFile = fbCreds
            }
        }
        release {
            isMinifyEnabled = false
            firebaseAppDistribution {
                appId = fbAppId
                artifactType = "APK"
                testers = "<your-email>"
                releaseNotesFile = "${rootDir}/release-notes.txt"
                serviceCredentialsFile = fbCreds
            }
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
