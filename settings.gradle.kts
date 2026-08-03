// Gradle settings for the Android half of the project.
//
// The C++ half is CMake-first (ADR-0013): Gradle's externalNativeBuild consumes
// platform/android/CMakeLists.txt, which in turn adds core/ and harness/ unchanged. Gradle is
// not the source of truth for the native build and must not become one.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "fileflow"
include(":app")
