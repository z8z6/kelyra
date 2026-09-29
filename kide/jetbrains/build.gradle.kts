import org.jetbrains.intellij.platform.gradle.tasks.PrepareSandboxTask

plugins {
  java
  id("org.jetbrains.intellij.platform") version "2.19.0"
}

group = "com.z8z6"
version = "0.4.1"

repositories {
  mavenCentral()
  intellijPlatform {
    defaultRepositories()
  }
}

dependencies {
  intellijPlatform {
    val localIdea = providers.environmentVariable("KIDE_IDEA_HOME").orNull
    if (localIdea != null) {
      local(localIdea)
    } else {
      intellijIdea("2025.2.6.1")
    }
    bundledPlugin("org.jetbrains.plugins.textmate")
    bundledModule("com.intellij.modules.lsp")
  }
}

intellijPlatform {
  buildSearchableOptions = false
  pluginConfiguration {
    ideaVersion {
      sinceBuild = "252"
    }
  }
}

tasks {
  // The plugin has no Swing forms to instrument.
  named("instrumentCode") { enabled = false }
  withType<JavaCompile> {
    sourceCompatibility = "21"
    targetCompatibility = "21"
    providers.environmentVariable("KIDE_JAVAC").orNull?.let {
      options.isFork = true
      options.forkOptions.executable = it
    }
  }
  withType<PrepareSandboxTask> {
    from(layout.projectDirectory.dir("textmate")) {
      into(pluginName.map { "$it/textmate" })
    }
    from(layout.projectDirectory.dir("../vscode/syntaxes")) {
      into(pluginName.map { "$it/textmate/syntaxes" })
    }
    from(layout.projectDirectory.file("../vscode/language-configuration.json")) {
      into(pluginName.map { "$it/textmate" })
    }
  }
}
