plugins {
    id("com.android.application") version "8.5.2" apply false
    id("org.jetbrains.kotlin.android") version "1.9.24" apply false
    // AppTester (Firebase App Distribution) 配信。`./gradlew :app:appDistributionUploadDebug`
    id("com.google.firebase.appdistribution") version "5.0.0" apply false
}
