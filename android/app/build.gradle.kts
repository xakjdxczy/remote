import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val keystoreProps = Properties()
val keystorePropsFile = rootProject.file("keystore.properties")
if (keystorePropsFile.isFile) {
    keystorePropsFile.inputStream().use { keystoreProps.load(it) }
}

fun signingValue(key: String, env: String): String =
    keystoreProps.getProperty(key)?.takeIf { it.isNotBlank() } ?: (System.getenv(env) ?: "")

android {
    namespace = "com.dustx.remotedesk"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.dustx.remotedesk"
        minSdk = 29
        targetSdk = 34
        versionCode = 22
        versionName = "1.9.6"
    }

    val releaseStoreFile = signingValue("storeFile", "DUSTX_ANDROID_STORE_FILE")
    val releaseStorePassword = signingValue("storePassword", "DUSTX_ANDROID_STORE_PASSWORD")
    val releaseKeyAlias = signingValue("keyAlias", "DUSTX_ANDROID_KEY_ALIAS")
    val releaseKeyPassword = signingValue("keyPassword", "DUSTX_ANDROID_KEY_PASSWORD")
    val hasReleaseSigning = releaseStoreFile.isNotEmpty() &&
        releaseStorePassword.isNotEmpty() &&
        releaseKeyAlias.isNotEmpty() &&
        releaseKeyPassword.isNotEmpty()

    if (hasReleaseSigning) {
        signingConfigs {
            create("release") {
                storeFile = rootProject.file(releaseStoreFile)
                storePassword = releaseStorePassword
                keyAlias = releaseKeyAlias
                keyPassword = releaseKeyPassword
            }
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
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
    buildFeatures {
        buildConfig = true
    }
    sourceSets["main"].java.srcDirs("src/main/kotlin")
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("io.getstream:stream-webrtc-android:1.3.8")
}
